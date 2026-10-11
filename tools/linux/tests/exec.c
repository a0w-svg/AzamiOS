#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include "exec-interpreter.inc"

static int passed, failed, azami, fd;
static char fixture[256], binary[512], interpreter[512], absent[512];
static volatile sig_atomic_t signals;
static _Thread_local volatile unsigned tls_value = 0x12345678;
static unsigned char *resident, *demand;
static unsigned char alternate_stack[16384];
static long old_break;
static char maps_before[32768], maps_after[32768];
static size_t maps_size;
static int stop_thread;
static unsigned steps;
static void record(const char *name, long value, long expected)
{
    int good = value == expected;
    printf("NSCASE {\"name\":\"%s\",\"value\":%ld,\"expected\":%ld,\"passed\":%s}\n",
           name, value, expected, good ? "true" : "false");
    if (good) passed++; else failed++;
}
static void fatal(const char *why) { perror(why); exit(2); }
static void write_file(const char *path, const void *data, size_t size, mode_t mode)
{
    int f = open(path, O_CREAT | O_TRUNC | O_WRONLY, mode);
    if (f < 0 || write(f, data, size) != (ssize_t)size || close(f) || chmod(path, mode)) fatal(path);
}
static void elf_header(Elf64_Ehdr *h, int type, unsigned phnum)
{
    memset(h, 0, sizeof(*h));
    memcpy(h->e_ident, ELFMAG, SELFMAG);
    h->e_ident[EI_CLASS] = ELFCLASS64; h->e_ident[EI_DATA] = ELFDATA2LSB;
    h->e_ident[EI_VERSION] = EV_CURRENT;
    h->e_type = type; h->e_machine = EM_X86_64; h->e_version = EV_CURRENT;
    h->e_ehsize = sizeof(*h); h->e_phoff = sizeof(*h);
    h->e_phentsize = sizeof(Elf64_Phdr); h->e_phnum = phnum;
    h->e_entry = (type == ET_DYN ? 0 : 0x400000) + 512;
}
static void make_main(const void *interp, size_t interp_size, uint64_t offset)
{
    unsigned char data[8192] = {0};
    Elf64_Ehdr *h = (void *)data;
    elf_header(h, ET_EXEC, interp ? 2 : 1);
    Elf64_Phdr *ph = (void *)(data + sizeof(*h));
    ph[0] = (Elf64_Phdr){.p_type=PT_LOAD, .p_flags=PF_R|PF_X,
        .p_vaddr=0x400000, .p_filesz=sizeof(data), .p_memsz=sizeof(data), .p_align=4096};
    /* mov $42,%edi; mov $60,%eax; syscall; ud2 */
    const unsigned char code[] = {0xbf,42,0,0,0,0xb8,60,0,0,0,0x0f,0x05,0x0f,0x0b};
    memcpy(data + 512, code, sizeof(code));
    if (interp) {
        ph[1] = (Elf64_Phdr){.p_type=PT_INTERP, .p_offset=offset, .p_filesz=interp_size};
        if (offset < sizeof(data) && interp_size <= sizeof(data) - offset)
            memcpy(data + offset, interp, interp_size);
    }
    write_file(binary, data, sizeof(data), 0700);
}
static void make_interpreter(int type)
{
    unsigned char data[4096] = {0};
    Elf64_Ehdr *h = (void *)data;
    elf_header(h, type, 1);
    Elf64_Phdr *ph = (void *)(data + sizeof(*h));
    ph[0] = (Elf64_Phdr){.p_type=PT_LOAD, .p_flags=PF_R|PF_X,
        .p_vaddr=type == ET_DYN ? 0 : 0x500000, .p_filesz=sizeof(data),
        .p_memsz=sizeof(data), .p_align=4096};
    if (type == ET_EXEC) h->e_entry = 0x500200;
    memcpy(data + 512, exec_interpreter, sizeof(exec_interpreter));
    write_file(interpreter, data, sizeof(data), 0700);
}
static void patch_interp(size_t offset, const void *data, size_t length)
{
    int f = open(interpreter, O_WRONLY);
    if (f < 0 || pwrite(f, data, length, offset) != (ssize_t)length || close(f)) fatal("patch interpreter");
}
static void read_maps(char *data, size_t *length)
{
    int f = open("/proc/self/maps", O_RDONLY);
    if (f < 0) fatal("maps");
    *length = 0;
    ssize_t n;
    while ((n = read(f, data + *length, 32768 - *length)) > 0) {
        *length += (size_t)n;
        if (*length == 32768) fatal("maps too big");
    }
    if (n < 0 || close(f)) fatal("read maps");
}
static void handler(int signo) { (void)signo; signals++; }
static long preservation(void)
{
    size_t n; read_maps(maps_after, &n);
    int good = n == maps_size && !memcmp(maps_before, maps_after, n);
    good &= resident[0] == 0xa5 && tls_value == 0x12345678;
    good &= fcntl(fd, F_GETFD) == FD_CLOEXEC;
    struct sigaction action;
    good &= sigaction(SIGUSR1, NULL, &action) == 0 && action.sa_handler == handler;
    stack_t stack;
    good &= sigaltstack(NULL, &stack) == 0 && stack.ss_sp == alternate_stack &&
            stack.ss_size == sizeof(alternate_stack) && stack.ss_flags == 0;
    good &= syscall(SYS_brk, 0) == old_break;
    good &= mprotect(resident, 4096, PROT_READ) == 0;
    good &= mprotect(resident, 4096, PROT_READ | PROT_WRITE) == 0;
    resident[0] = 0xa5;
    return good;
}
static long exec_error(const char *path, const void *argv, const void *envp)
{
    errno = 0;
    long value = syscall(SYS_execve, path, argv, envp);
    return value < 0 ? -errno : value;
}
static void fail_case(const char *name, long expected)
{
    char *args[] = {binary, "argument", NULL}; char *env[] = {"EXEC_TEST=yes", NULL};
    long error = exec_error(binary, args, env);
    record(name, error, expected);
    char check[128]; snprintf(check, sizeof(check), "%s.preserved", name);
    record(check, preservation(), 1);
}
static long child_exec(const char *path)
{
    pid_t child = fork();
    if (child < 0) return -errno;
    if (!child) {
        if (dup2(fd, 32) != 32 || dup2(fd, 33) != 33 ||
            fcntl(32, F_SETFD, FD_CLOEXEC) || fcntl(33, F_SETFD, 0)) _exit(101);
        char *args[] = {(char *)path, "argument", NULL}; char *env[] = {"EXEC_TEST=yes", NULL};
        execve(path, args, env); _exit(100);
    }
    int status;
    if (waitpid(child, &status, 0) != child) return -errno;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -(1000 + WTERMSIG(status));
}
static void *worker(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&stop_thread, __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&steps, 1, __ATOMIC_RELAXED);
        sched_yield();
    }
    return NULL;
}
static int cleanup(const char *path, const struct stat *st, int type, struct FTW *walk)
{ (void)st; (void)type; (void)walk; return remove(path); }
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    struct utsname uts; azami = !uname(&uts) && !strcmp(uts.sysname, "AzamiOS");
    strcpy(fixture, "/tmp/azami-exec-XXXXXX"); if (!mkdtemp(fixture)) fatal("mkdtemp");
    snprintf(binary, sizeof(binary), "%s/main", fixture);
    snprintf(interpreter, sizeof(interpreter), "%s/loader", fixture);
    snprintf(absent, sizeof(absent), "%s/missing", fixture);
    resident = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    demand = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (resident == MAP_FAILED || demand == MAP_FAILED) fatal("mmap");
    resident[0] = 0xa5;
    char fd_path[512]; snprintf(fd_path, sizeof(fd_path), "%s/descriptor", fixture);
    fd = open(fd_path, O_CREAT | O_RDWR | O_CLOEXEC, 0700); if (fd < 0) fatal("fd");
    struct sigaction action = {.sa_handler=handler};
    if (sigaction(SIGUSR1, &action, NULL)) fatal("signal");
    stack_t stack = {.ss_sp=alternate_stack, .ss_size=sizeof(alternate_stack)};
    if (sigaltstack(&stack, NULL)) fatal("alternate signal stack");
    old_break = syscall(SYS_brk, 0);
    read_maps(maps_before, &maps_size);
    make_main(absent, strlen(absent) + 1, 1024);
    fail_case("interp.missing", -ENOENT);
    char alias[600]; snprintf(alias, sizeof(alias), "%s.elf", absent);
    make_interpreter(ET_DYN);
    if (link(interpreter, alias)) fatal("alias");
    fail_case("interp.no_suffix_substitution", -ENOENT);
    unlink(alias);
    make_main(interpreter, strlen(interpreter) + 1, 1024);
    record("interp.exact_auxv", child_exec(binary), 41);
    record("exec.failure_keeps_parent", preservation(), 1);
    char deep[512]; strcpy(deep, fixture);
    while (strlen(deep) < 280) {
        strcat(deep, "/directory-with-a-long-component-to-exercise-path-size");
        if (mkdir(deep, 0700)) fatal("long path directory");
    }
    strcat(deep, "/loader");
    if (link(interpreter, deep)) fatal("long path loader");
    make_main(deep, strlen(deep) + 1, 1024);
    record("interp.path_over_256", child_exec(binary), 41);
    char cwd[512];
    if (!getcwd(cwd, sizeof(cwd)) || chdir(fixture)) fatal("relative cwd");
    make_main("loader", 7, 1024);
    record("interp.relative_cwd", child_exec(binary), 41);
    if (chdir(cwd)) fatal("restore cwd");
    if (chdir(azami ? "/test-exec" : "tools/linux/out/bin")) fatal("musl fixture cwd");
    record("interp.real_musl", child_exec("./azami-exec-dynamic"), 43);
    if (chdir(cwd)) fatal("restore musl cwd");
    make_main(NULL, 0, 0); record("exec.static", child_exec(binary), 42);
    make_main(interpreter, strlen(interpreter) + 1, 1024);
    uint32_t zero = 0; patch_interp(0, &zero, sizeof(zero));
    fail_case("interp.bad_magic", -ELIBBAD);
    make_interpreter(ET_DYN); uint16_t machine = EM_AARCH64;
    patch_interp(__builtin_offsetof(Elf64_Ehdr, e_machine), &machine, sizeof(machine));
    fail_case("interp.wrong_machine", -ELIBBAD);
    make_interpreter(ET_DYN); uint16_t phent = 1;
    patch_interp(__builtin_offsetof(Elf64_Ehdr, e_phentsize), &phent, sizeof(phent));
    fail_case("interp.bad_phentsize", -ELIBBAD);
    make_interpreter(ET_DYN); if (truncate(interpreter, 16)) fatal("truncate");
    fail_case("interp.short_header", -EIO);
    make_interpreter(ET_DYN); if (truncate(interpreter, sizeof(Elf64_Ehdr))) fatal("truncate");
    fail_case("interp.short_phdrs", -ELIBBAD);
    make_interpreter(ET_DYN); if (chmod(interpreter, 0600)) fatal("chmod");
    fail_case("interp.not_executable", -EACCES);
    make_main(fixture, strlen(fixture) + 1, 1024); fail_case("interp.directory", -EACCES);
    make_main(interpreter, strlen(interpreter), 1024); fail_case("interp.unterminated", -ENOEXEC);
    make_main("", 1, 1024); fail_case("interp.too_short", -ENOEXEC);
    char large[4097]; memset(large, 'x', sizeof(large)); large[4096] = 0;
    make_main(large, sizeof(large), 1024); fail_case("interp.oversized", -ENOEXEC);
    make_main(interpreter, strlen(interpreter) + 1, 8190); fail_case("interp.short_path", -EIO);
    make_main(large, 512, 1024); fail_case("interp.long_component", -ENOEXEC);
    large[511] = 0; make_main(large, 512, 1024); fail_case("interp.component_limit", -ENAMETOOLONG);
    if (unlink(interpreter) || symlink("loader", interpreter)) fatal("symlink");
    make_main(interpreter, strlen(interpreter) + 1, 1024); fail_case("interp.symlink_loop", -ELOOP);
    unlink(interpreter); make_interpreter(ET_DYN);
    snprintf(alias, sizeof(alias), "%s/child", interpreter);
    make_main(alias, strlen(alias) + 1, 1024); fail_case("interp.not_directory", -ENOTDIR);
    /* These errors happen after Linux has committed exec and killed the old
     * image. AzamiOS stages eagerly and can return them with the caller intact. */
    if (azami) {
        make_main(interpreter, strlen(interpreter) + 1, 1024);
        make_interpreter(ET_DYN); uint8_t cls = ELFCLASS32;
        patch_interp(EI_CLASS, &cls, 1); fail_case("staged.wrong_class", -ELIBBAD);
        make_interpreter(ET_DYN); uint8_t endian = ELFDATA2MSB;
        patch_interp(EI_DATA, &endian, 1); fail_case("staged.wrong_endian", -ELIBBAD);
        make_interpreter(ET_DYN); uint64_t filesz = 8192;
        patch_interp(sizeof(Elf64_Ehdr) + __builtin_offsetof(Elf64_Phdr, p_filesz), &filesz, 8);
        fail_case("staged.filesz_gt_memsz", -ELIBBAD);
        make_interpreter(ET_DYN); uint64_t offset = 4096;
        patch_interp(sizeof(Elf64_Ehdr) + __builtin_offsetof(Elf64_Phdr, p_offset), &offset, 8);
        fail_case("staged.segment_short_read", -EIO);
        make_interpreter(ET_EXEC); uint64_t addr = 0xfffffffffffff000ULL;
        patch_interp(sizeof(Elf64_Ehdr) + __builtin_offsetof(Elf64_Phdr, p_vaddr), &addr, 8);
        fail_case("staged.address_overflow", -EINVAL);
        make_interpreter(ET_EXEC); addr = 0x400000;
        patch_interp(sizeof(Elf64_Ehdr) + __builtin_offsetof(Elf64_Phdr, p_vaddr), &addr, 8);
        fail_case("staged.image_collision", -EINVAL);
        /* Reach stack construction after the ELF, initial stack pages and
         * vDSO have been mapped, then force a bounded argument-size failure. */
        make_main(NULL, 0, 0);
        char long_arg[1023]; memset(long_arg, 'x', sizeof(long_arg) - 1); long_arg[1022] = 0;
        char *large_argv[] = {binary, long_arg, long_arg, long_arg, long_arg, NULL};
        char *empty_env[] = {NULL};
        record("staged.stack_argument_limit", exec_error(binary, large_argv, empty_env), -E2BIG);
        record("staged.stack_argument_limit.preserved", preservation(), 1);
    } else puts("NSSKIP {\"name\":\"staged_errors\",\"reason\":\"Malformed-image validation and commit timing differ; AzamiOS also bounds argument storage\"}");
    make_main(NULL, 0, 0);
    char suffix[600]; snprintf(suffix, sizeof(suffix), "%s.elf", binary);
    if (rename(binary, suffix)) fatal("suffix");
    fail_case("exec.no_suffix_substitution", -ENOENT);
    if (rename(suffix, binary)) fatal("restore");
    char *args[] = {binary, NULL}; char *env[] = {NULL};
    record("exec.argv_fault", exec_error(binary, (void *)1, env), -EFAULT);
    record("exec.envp_fault", exec_error(binary, args, (void *)1), -EFAULT);
    char *bad[] = {(void *)1, NULL};
    record("exec.argument_fault", exec_error(binary, bad, env), -EFAULT);
    record("exec.path_fault", exec_error((void *)1, args, env), -EFAULT);
    record("exec.null_path", exec_error(NULL, args, env), -EFAULT);
    record("exec.argument_failures_preserved", preservation(), 1);
    const char script[] = "#!/definitely/missing/azami-shell\nexit 0\n";
    write_file(binary, script, sizeof(script)-1, 0700); fail_case("script.missing_interpreter", -ENOENT);
    record("exec.signal_survives", !kill(getpid(), SIGUSR1) && signals == 1, 1);
    make_main(absent, strlen(absent) + 1, 1024);
    pthread_t thread;
    int started = pthread_create(&thread, NULL, worker, NULL) == 0;
    record("exec.thread_started", started, 1);
    if (started) {
        while (!__atomic_load_n(&steps, __ATOMIC_RELAXED)) sched_yield();
        unsigned before = __atomic_load_n(&steps, __ATOMIC_RELAXED);
        int good = 1;
        for (int i = 0; i < 100; i++) good &= exec_error(binary, args, env) == -ENOENT;
        while (__atomic_load_n(&steps, __ATOMIC_RELAXED) == before) sched_yield();
        __atomic_store_n(&stop_thread, 1, __ATOMIC_RELEASE);
        good &= !pthread_join(thread, NULL);
        record("exec.repeated_failure_threads", good, 1);
    }
    record("exec.repeated_failure_preserved", preservation(), 1);
    demand[0] = 0x5a; record("exec.demand_fault_survives", demand[0], 0x5a);
    close(fd); munmap(resident, 4096); munmap(demand, 4096);
    record("fixture.cleanup", nftw(fixture, cleanup, 16, FTW_DEPTH | FTW_PHYS) == 0, 1);
    printf("== namespaces skipped: %d ==\n", azami ? 0 : 1);
    printf("== probe complete: %d passed, %d failed ==\n", passed, failed);
    return failed != 0;
}
