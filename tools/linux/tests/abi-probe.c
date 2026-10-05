/* Linux-ABI conformance probe, built with a stock Linux toolchain (musl).
 * Nothing here is Azami-specific: it is the set of things a real Linux program
 * does on its way to doing useful work. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/auxv.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <sys/timex.h>
#include <sched.h>
#include <elf.h>

#ifndef SYS_pivot_root
#define SYS_pivot_root 155
#endif
#ifndef SYS_unshare
#define SYS_unshare 272
#endif
#ifndef SYS_epoll_create1
#define SYS_epoll_create1 291
#endif
#ifndef SYS_openat2
#define SYS_openat2 437
#endif
#ifndef SYS_epoll_pwait2
#define SYS_epoll_pwait2 441
#endif

static int pass, fail;
#define T(cond, name) do { \
    if (cond) { printf("  ok   %s\n", name); pass++; } \
    else { printf("  FAIL %s (errno=%d %s)\n", name, errno, strerror(errno)); fail++; } \
} while (0)

static void *thread_fn(void *arg) { return (void *)((long)arg + 1); }

static long long ts_ns(const struct timespec *t) { return t->tv_sec * 1000000000LL + t->tv_nsec; }

/* Resolve a symbol in the vDSO the way libc does: walk its dynamic section. */
static void *vdso_sym(const char *name)
{
    unsigned long base = getauxval(AT_SYSINFO_EHDR);
    if (!base) return NULL;
    Elf64_Ehdr *eh = (Elf64_Ehdr *)base;
    Elf64_Phdr *ph = (Elf64_Phdr *)(base + eh->e_phoff);
    Elf64_Dyn *dyn = NULL;
    long load = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) load = base + ph[i].p_offset - ph[i].p_vaddr;
        if (ph[i].p_type == PT_DYNAMIC) dyn = (Elf64_Dyn *)(base + ph[i].p_offset);
    }
    if (!dyn) return NULL;
    Elf64_Sym *sym = NULL; const char *str = NULL; Elf32_Word *hash = NULL;
    for (; dyn->d_tag != DT_NULL; dyn++) {
        if (dyn->d_tag == DT_SYMTAB) sym  = (Elf64_Sym *)(load + dyn->d_un.d_ptr);
        if (dyn->d_tag == DT_STRTAB) str  = (const char *)(load + dyn->d_un.d_ptr);
        if (dyn->d_tag == DT_HASH)   hash = (Elf32_Word *)(load + dyn->d_un.d_ptr);
    }
    if (!sym || !str || !hash) return NULL;
    for (Elf32_Word i = 0; i < hash[1]; i++)
        if (sym[i].st_shndx != SHN_UNDEF && strcmp(str + sym[i].st_name, name) == 0)
            return (void *)(load + sym[i].st_value);
    return NULL;
}
static volatile sig_atomic_t got_sig;
static void handler(int s) { got_sig = s; }

int main(int argc, char **argv, char **envp)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("== AzamiOS Linux-ABI probe (musl static) ==\n");
    printf("argc=%d argv0=%s\n", argc, argv[0] ? argv[0] : "(null)");
    printf("AT_PHDR=%#lx AT_PHNUM=%lu AT_PAGESZ=%lu AT_ENTRY=%#lx\n",
           getauxval(AT_PHDR), getauxval(AT_PHNUM),
           getauxval(AT_PAGESZ), getauxval(AT_ENTRY));

    printf("-- process/ids --\n");
    T(getpid() > 0, "getpid");
    T(getppid() >= 0, "getppid");
    T(getuid() >= 0, "getuid");

    printf("-- syscall ABI / register preservation --\n");
    unsigned long canaries[6] = {
        0x1122334455667788ULL,
        0x2233445566778899ULL,
        0x33445566778899aaULL,
        0x445566778899aabbULL,
        0x5566778899aabbccULL,
        0x66778899aabbccddULL,
    };
    unsigned long observed[6] = {0};
    unsigned long ret_pid = 0;
    __asm__ volatile (
        "push %%rbx\n\t"
        "push %%rbp\n\t"
        "mov 0(%[in]), %%rbx\n\t"
        "mov 8(%[in]), %%rbp\n\t"
        "mov 16(%[in]), %%r12\n\t"
        "mov 24(%[in]), %%r13\n\t"
        "mov 32(%[in]), %%r14\n\t"
        "mov 40(%[in]), %%r15\n\t"
        "syscall\n\t"
        "mov %%rbx, 0(%[out])\n\t"
        "mov %%rbp, 8(%[out])\n\t"
        "mov %%r12, 16(%[out])\n\t"
        "mov %%r13, 24(%[out])\n\t"
        "mov %%r14, 32(%[out])\n\t"
        "mov %%r15, 40(%[out])\n\t"
        "pop %%rbp\n\t"
        "pop %%rbx\n\t"
        : "=a"(ret_pid)
        : "a"((unsigned long)SYS_getpid),
          [in] "r"(canaries),
          [out] "r"(observed)
        : "rcx", "r11", "r12", "r13", "r14", "r15", "memory"
    );
    T(ret_pid == (unsigned long)getpid(), "syscall SYS_getpid returns correct PID");
    T(observed[0] == canaries[0], "callee-saved rbx preserved across syscall");
    T(observed[1] == canaries[1], "callee-saved rbp preserved across syscall");
    T(observed[2] == canaries[2], "callee-saved r12 preserved across syscall");
    T(observed[3] == canaries[3], "callee-saved r13 preserved across syscall");
    T(observed[4] == canaries[4], "callee-saved r14 preserved across syscall");
    T(observed[5] == canaries[5], "callee-saved r15 preserved across syscall");

    /* Test 6-argument raw syscall */
    long mmap_res;
    __asm__ volatile (
        "mov %[arg4], %%r10\n\t"
        "mov %[arg5], %%r8\n\t"
        "mov %[arg6], %%r9\n\t"
        "syscall\n\t"
        : "=a"(mmap_res)
        : "a"((unsigned long)SYS_mmap),
          "D"(0UL),
          "S"(4096UL),
          "d"((unsigned long)(PROT_READ | PROT_WRITE)),
          [arg4] "r"((unsigned long)(MAP_PRIVATE | MAP_ANONYMOUS)),
          [arg5] "r"((unsigned long)-1),
          [arg6] "r"(0UL)
        : "rcx", "r11", "r10", "r8", "r9", "memory"
    );
    T(mmap_res > 0 && (mmap_res & 0xfff) == 0, "6-argument raw syscall mmap succeeds");
    if (mmap_res > 0 && (mmap_res & 0xfff) == 0) {
        munmap((void *)mmap_res, 4096);
    }

    /* Test invalid syscall number error return */
    long bad_res;
    __asm__ volatile (
        "syscall\n\t"
        : "=a"(bad_res)
        : "a"(9999UL)
        : "rcx", "r11", "memory"
    );
    T(bad_res == -ENOSYS, "invalid syscall nr returns -ENOSYS");

    printf("-- uname --\n");
    struct utsname u;
    if (uname(&u) == 0)
        printf("  %s %s %s %s\n", u.sysname, u.nodename, u.release, u.machine);
    T(uname(&u) == 0, "uname");

    printf("-- memory --\n");
    void *p = malloc(1 << 20);
    T(p != NULL, "malloc 1MB");
    if (p) { memset(p, 0xAB, 1 << 20); T(((unsigned char *)p)[1000] == 0xAB, "malloc write/read"); free(p); }
    void *m = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    T(m != MAP_FAILED, "mmap anon 64K");
    if (m != MAP_FAILED) {
        memset(m, 1, 65536);
        T(mprotect(m, 65536, PROT_READ) == 0, "mprotect RO");
        T(munmap(m, 65536) == 0, "munmap");
    }

    printf("-- filesystem --\n");
    char cwd[256];
    T(getcwd(cwd, sizeof cwd) != NULL, "getcwd");
    printf("  cwd=%s\n", cwd);
    int fd = open("/tmp/probe.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) fd = open("/probe.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    T(fd >= 0, "open O_CREAT");
    if (fd >= 0) {
        const char *msg = "linux binary wrote this\n";
        T(write(fd, msg, strlen(msg)) == (ssize_t)strlen(msg), "write");
        T(lseek(fd, 0, SEEK_SET) == 0, "lseek");
        char rb[64] = {0};
        T(read(fd, rb, sizeof rb - 1) > 0, "read back");
        T(strcmp(rb, msg) == 0, "read matches write");
        struct stat st;
        T(fstat(fd, &st) == 0, "fstat");
        T(st.st_size == (off_t)strlen(msg), "st_size correct");
        T(close(fd) == 0, "close");
    }
    DIR *d = opendir("/");
    T(d != NULL, "opendir /");
    if (d) {
        int n = 0;
        struct dirent *de;
        while ((de = readdir(d))) n++;
        printf("  / has %d entries\n", n);
        T(n > 2, "readdir returns entries");
        closedir(d);
    }

    printf("-- stdio buffering --\n");
    FILE *f = fopen("/tmp/probe2.txt", "w");
    if (!f) f = fopen("/probe2.txt", "w");
    T(f != NULL, "fopen w");
    if (f) { fprintf(f, "%d %s\n", 42, "buffered"); T(fclose(f) == 0, "fclose flushes"); }
    T(isatty(1) == 0 || isatty(1) == 1, "isatty does not crash");

    printf("-- time --\n");
    struct timespec ts;
    T(clock_gettime(CLOCK_REALTIME, &ts) == 0, "clock_gettime REALTIME");
    T(clock_gettime(CLOCK_MONOTONIC, &ts) == 0, "clock_gettime MONOTONIC");
    time_t t0 = time(NULL);
    T(t0 > 0, "time()");
    struct timespec req = { 0, 20 * 1000 * 1000 };
    T(nanosleep(&req, NULL) == 0, "nanosleep 20ms");

    printf("-- vDSO / clocks --\n");
    T(getauxval(AT_SYSINFO_EHDR) != 0, "AT_SYSINFO_EHDR present");
    int (*vgt)(clockid_t, struct timespec *) = vdso_sym("__vdso_clock_gettime");
    T(vgt != NULL, "vdso exports __vdso_clock_gettime");
    T(vdso_sym("__vdso_gettimeofday") && vdso_sym("__vdso_getcpu") && vdso_sym("__vdso_time"),
      "vdso exports gettimeofday/getcpu/time");
    if (vgt) {
        struct timespec a, b, k;
        T(vgt(CLOCK_MONOTONIC, &a) == 0, "vdso clock_gettime MONOTONIC");
        syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &k);
        T(vgt(CLOCK_MONOTONIC, &b) == 0 && ts_ns(&a) <= ts_ns(&k) && ts_ns(&k) <= ts_ns(&b),
          "vdso and syscall MONOTONIC agree and are ordered");
        struct timespec vr, sr;
        vgt(CLOCK_REALTIME, &vr);
        syscall(SYS_clock_gettime, CLOCK_REALTIME, &sr);
        long long d = ts_ns(&sr) - ts_ns(&vr);
        T(d >= 0 && d < 50000000LL, "vdso and syscall REALTIME agree (<50ms)");
        T(vr.tv_sec > 1600000000L, "REALTIME is a plausible date");
        /* 100k calls: fast path must not be a disguised syscall per call. */
        struct timespec t0, t1, tmp;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        long long prev = 0; int mono_ok = 1;
        for (int i = 0; i < 100000; i++) {
            vgt(CLOCK_MONOTONIC, &tmp);
            long long n = ts_ns(&tmp);
            if (n < prev) mono_ok = 0;
            prev = n;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        T(mono_ok, "MONOTONIC never goes backwards (100k reads)");
        printf("  vdso clock_gettime: %lld ns/call\n", (ts_ns(&t1) - ts_ns(&t0)) / 100000);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < 20000; i++) syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &tmp);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        printf("  syscall clock_gettime: %lld ns/call\n", (ts_ns(&t1) - ts_ns(&t0)) / 20000);
    }
    struct timespec c1, c2;
    T(clock_gettime(CLOCK_MONOTONIC_COARSE, &c1) == 0, "clock_gettime MONOTONIC_COARSE");
    T(clock_gettime(CLOCK_MONOTONIC_RAW, &c1) == 0, "clock_gettime MONOTONIC_RAW");
    T(clock_gettime(CLOCK_BOOTTIME, &c1) == 0, "clock_gettime BOOTTIME");
    T(clock_gettime(CLOCK_TAI, &c1) == 0, "clock_gettime TAI");
    T(clock_gettime(12345, &c1) == -1 && errno == EINVAL, "clock_gettime bad id -> EINVAL");
    T(clock_getres(CLOCK_MONOTONIC, &c1) == 0 && c1.tv_sec == 0 && c1.tv_nsec > 0 && c1.tv_nsec <= 1000,
      "clock_getres MONOTONIC is high-resolution");
    struct timeval tv;
    T(gettimeofday(&tv, NULL) == 0 && tv.tv_sec > 1600000000L, "gettimeofday");
    unsigned cpu = 999, node = 999;
    long (*vgc)(unsigned *, unsigned *, void *) = vdso_sym("__vdso_getcpu");
    T(vgc && vgc(&cpu, &node, NULL) == 0 && cpu < 64 && node == 0, "vdso getcpu");
    unsigned scpu = 999;
    T(syscall(SYS_getcpu, &scpu, NULL, NULL) == 0 && scpu < 64, "getcpu syscall");
    T(sched_getcpu() >= 0, "sched_getcpu");

    /* CPU-time clocks: spin ~30ms and require the thread clock to advance
     * by a sensible amount (not 0, not wall time). */
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c1);
    struct timespec w0, w1;
    clock_gettime(CLOCK_MONOTONIC, &w0);
    do { clock_gettime(CLOCK_MONOTONIC, &w1); } while (ts_ns(&w1) - ts_ns(&w0) < 30000000LL);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c2);
    long long used = ts_ns(&c2) - ts_ns(&c1);
    printf("  thread cputime over 30ms spin: %lld us\n", used / 1000);
    T(used > 5000000LL && used <= ts_ns(&w1) - ts_ns(&w0) + 1000000LL, "THREAD_CPUTIME advances while running");
    T(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c2) == 0 && ts_ns(&c2) >= used, "PROCESS_CPUTIME >= thread time");
    clockid_t pcid;
    T(clock_getcpuclockid(getpid(), &pcid) == 0 && clock_gettime(pcid, &c1) == 0, "clock_getcpuclockid(getpid())");

    /* Sleeps: never short, and a 1ms sleep must not cost a whole 10ms tick. */
    struct timespec s0, s1, req1 = { 0, 1000000 };
    clock_gettime(CLOCK_MONOTONIC, &s0);
    T(nanosleep(&req1, NULL) == 0, "nanosleep 1ms");
    clock_gettime(CLOCK_MONOTONIC, &s1);
    long long slept = ts_ns(&s1) - ts_ns(&s0);
    printf("  nanosleep(1ms) took %lld us\n", slept / 1000);
    T(slept >= 1000000LL, "nanosleep never returns early");
    struct timespec abs_t;
    clock_gettime(CLOCK_MONOTONIC, &abs_t);
    long long target = ts_ns(&abs_t) + 15000000LL;
    abs_t.tv_sec = target / 1000000000LL; abs_t.tv_nsec = target % 1000000000LL;
    T(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &abs_t, NULL) == 0, "clock_nanosleep TIMER_ABSTIME");
    clock_gettime(CLOCK_MONOTONIC, &s1);
    T(ts_ns(&s1) >= target && ts_ns(&s1) - target < 100000000LL, "absolute sleep ends at the deadline");
    struct timespec past = { 1, 0 };
    T(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &past, NULL) == 0, "absolute sleep in the past returns at once");

    struct timex tx = { 0 };
    int st = adjtimex(&tx);
    T(st >= 0 && tx.tick == 10000, "adjtimex read-only query");

    printf("-- signals --\n");
    T(signal(SIGUSR1, handler) != SIG_ERR, "signal(SIGUSR1)");
    T(kill(getpid(), SIGUSR1) == 0, "kill self SIGUSR1");
    for (int i = 0; i < 1000000 && !got_sig; i++) ;
    T(got_sig == SIGUSR1, "handler ran");

    printf("-- threads --\n");
    pthread_t th;
    void *ret = NULL;
    int rc = pthread_create(&th, NULL, thread_fn, (void *)41L);
    T(rc == 0, "pthread_create");
    if (rc == 0) {
        T(pthread_join(th, &ret) == 0, "pthread_join");
        T((long)ret == 42, "thread return value");
    }

    printf("-- fork/exec --\n");
    pid_t child = fork();
    if (child == 0) { _exit(7); }
    T(child > 0, "fork");
    if (child > 0) {
        int status = 0;
        T(waitpid(child, &status, 0) == child, "waitpid");
        T(WIFEXITED(status) && WEXITSTATUS(status) == 7, "child exit status 7");
    }

    printf("-- links --\n");
    /* The regressions these cover are all ones stock Linux tools tripped over:
     * a link count of 0 makes find(1) stop descending, link(2) implemented as
     * a symlink makes `ln` lie, and an unlink(2) that follows its final
     * symlink makes `rm somelink` delete the target instead of the link. */
    const char *base = "/tmp/probe-links";
    if (mkdir(base, 0755) != 0 && errno != EEXIST) base = "/probe-links";
    mkdir(base, 0755);
    char orig[256], hard[256], soft[256];
    snprintf(orig, sizeof orig, "%s/orig", base);
    snprintf(hard, sizeof hard, "%s/hard", base);
    snprintf(soft, sizeof soft, "%s/soft", base);
    unlink(hard); unlink(soft); unlink(orig);

    int lf = open(orig, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    T(lf >= 0, "create link test file");
    if (lf >= 0) { write(lf, "payload\n", 8); close(lf); }

    struct stat ls1;
    T(stat(orig, &ls1) == 0, "stat new file");
    T(ls1.st_nlink == 1, "st_nlink == 1 for a fresh file");
    T(ls1.st_blksize > 0, "st_blksize is nonzero");

    T(link(orig, hard) == 0, "link() creates a hard link");
    struct stat ls2, ls3;
    T(stat(hard, &ls2) == 0, "stat hard link");
    T(!S_ISLNK(ls2.st_mode), "hard link is not a symlink");
    T(ls2.st_ino == ls1.st_ino, "hard link shares the inode");
    T(stat(orig, &ls3) == 0 && ls3.st_nlink == 2, "st_nlink == 2 after link()");
    T(unlink(hard) == 0, "unlink the hard link");
    T(stat(orig, &ls3) == 0, "original survives unlinking its hard link");
    T(stat(orig, &ls3) == 0 && ls3.st_nlink == 1, "st_nlink back to 1");

    T(symlink("orig", soft) == 0, "symlink()");
    char lbuf[64] = {0};
    T(readlink(soft, lbuf, sizeof lbuf - 1) == 4 && strcmp(lbuf, "orig") == 0, "readlink()");
    struct stat sl;
    T(lstat(soft, &sl) == 0 && S_ISLNK(sl.st_mode), "lstat sees the symlink");
    T(stat(soft, &sl) == 0 && S_ISREG(sl.st_mode), "stat follows the symlink");
    T(unlink(soft) == 0, "unlink the symlink");
    T(stat(orig, &ls3) == 0, "unlink(symlink) left the target alone");

    struct stat ds;
    T(stat(base, &ds) == 0 && ds.st_nlink >= 2, "directory st_nlink >= 2");

    /* renameat2 testing */
    char r1[256], r2[256], r3[256];
    snprintf(r1, sizeof r1, "%s/r1", base);
    snprintf(r2, sizeof r2, "%s/r2", base);
    snprintf(r3, sizeof r3, "%s/r3", base);
    int f1 = open(r1, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int f2 = open(r2, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f1 >= 0) close(f1);
    if (f2 >= 0) close(f2);

    /* Test renameat2 with RENAME_NOREPLACE when target exists -> must fail with -EEXIST */
    long r_err = syscall(SYS_renameat2, AT_FDCWD, r1, AT_FDCWD, r2, 1 /* RENAME_NOREPLACE */);
    T(r_err == -1 && errno == EEXIST, "renameat2 RENAME_NOREPLACE fails when target exists with EEXIST");

    /* Test renameat2 with RENAME_NOREPLACE when target does not exist -> must succeed */
    r_err = syscall(SYS_renameat2, AT_FDCWD, r1, AT_FDCWD, r3, 1 /* RENAME_NOREPLACE */);
    T(r_err == 0, "renameat2 RENAME_NOREPLACE succeeds when target does not exist");
    T(access(r3, F_OK) == 0 && access(r1, F_OK) != 0, "renameat2 source moved to target");

    /* Test renameat2 with invalid flags -> must fail with -EINVAL */
    r_err = syscall(SYS_renameat2, AT_FDCWD, r3, AT_FDCWD, r1, 0xFFFFFFFF);
    T(r_err == -1 && errno == EINVAL, "renameat2 invalid flags fails with EINVAL");

    unlink(r2); unlink(r3);
    unlink(orig); rmdir(base);

    printf("-- procfs / sysfs fidelity --\n");
    FILE *fp = fopen("/proc/meminfo", "r");
    T(fp != NULL, "open /proc/meminfo");
    if (fp) {
        char buf[256];
        int has_total = 0, has_free = 0;
        while (fgets(buf, sizeof(buf), fp)) {
            if (strncmp(buf, "MemTotal:", 9) == 0) has_total = 1;
            if (strncmp(buf, "MemFree:", 8) == 0) has_free = 1;
        }
        fclose(fp);
        T(has_total && has_free, "parse /proc/meminfo MemTotal and MemFree");
    }

    fp = fopen("/proc/cpuinfo", "r");
    T(fp != NULL, "open /proc/cpuinfo");
    if (fp) {
        char buf[256];
        int has_cpu = 0;
        while (fgets(buf, sizeof(buf), fp)) {
            if (strncmp(buf, "processor", 9) == 0) has_cpu = 1;
        }
        fclose(fp);
        T(has_cpu, "parse /proc/cpuinfo processor entry");
    }

    fp = fopen("/proc/self/stat", "r");
    T(fp != NULL, "open /proc/self/stat");
    if (fp) {
        int spid = 0;
        char scomm[64] = {0};
        char sstate = 0;
        int parsed = fscanf(fp, "%d (%63[^)]) %c", &spid, scomm, &sstate);
        fclose(fp);
        T(parsed == 3 && spid == getpid(), "/proc/self/stat matches getpid()");
    }

    fp = fopen("/proc/self/status", "r");
    T(fp != NULL, "open /proc/self/status");
    if (fp) {
        char buf[256];
        int has_name = 0, has_state = 0, has_pid = 0;
        while (fgets(buf, sizeof(buf), fp)) {
            if (strncmp(buf, "Name:", 5) == 0) has_name = 1;
            if (strncmp(buf, "State:", 6) == 0) has_state = 1;
            if (strncmp(buf, "Pid:", 4) == 0) has_pid = 1;
        }
        fclose(fp);
        T(has_name && has_state && has_pid, "/proc/self/status has Name, State, Pid");
    }

    fp = fopen("/proc/self/maps", "r");
    T(fp != NULL, "open /proc/self/maps");
    if (fp) {
        char buf[256];
        int has_entries = (fgets(buf, sizeof(buf), fp) != NULL);
        fclose(fp);
        T(has_entries, "/proc/self/maps returns memory mappings");
    }

    printf("-- openat2 resolution fidelity --\n");
    char o2_dir[] = "/tmp/abi_o2_XXXXXX";
    char *o2_base = mkdtemp(o2_dir);
    T(o2_base != NULL, "mkdtemp for openat2 tests");
    if (o2_base) {
        struct {
            unsigned long long flags;
            unsigned long long mode;
            unsigned long long resolve;
        } how;
        memset(&how, 0, sizeof(how));
        how.flags = O_RDONLY;

        /* openat2 with size < sizeof(how) -> EINVAL */
        long o2_res = syscall(SYS_openat2, AT_FDCWD, "/proc/meminfo", &how, 4);
        T(o2_res == -1 && errno == EINVAL, "openat2 with size < sizeof(how) fails with EINVAL");

        /* openat2 with unknown resolve flag -> EINVAL */
        how.resolve = 0x80;
        o2_res = syscall(SYS_openat2, AT_FDCWD, "/proc/meminfo", &how, sizeof(how));
        T(o2_res == -1 && errno == EINVAL, "openat2 with unknown resolve flag fails with EINVAL");

        /* openat2 RESOLVE_BENEATH with absolute path -> EXDEV */
        how.resolve = 0x08 /* RESOLVE_BENEATH */;
        o2_res = syscall(SYS_openat2, AT_FDCWD, "/proc/meminfo", &how, sizeof(how));
        T(o2_res == -1 && errno == EXDEV, "openat2 RESOLVE_BENEATH with absolute path fails with EXDEV");

        /* openat2 RESOLVE_BENEATH with relative path escaping parent -> EXDEV */
        how.resolve = 0x08 /* RESOLVE_BENEATH */;
        o2_res = syscall(SYS_openat2, AT_FDCWD, "../../../../../etc/passwd", &how, sizeof(how));
        T(o2_res == -1 && errno == EXDEV, "openat2 RESOLVE_BENEATH escaping base fails with EXDEV");

        /* openat2 RESOLVE_NO_SYMLINKS with symlink -> ELOOP */
        char s_tgt[128], s_lnk[128];
        snprintf(s_tgt, sizeof s_tgt, "%s/target.txt", o2_base);
        snprintf(s_lnk, sizeof s_lnk, "%s/symlink.txt", o2_base);
        int s_fd = open(s_tgt, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (s_fd >= 0) close(s_fd);
        symlink(s_tgt, s_lnk);
        how.resolve = 0x04 /* RESOLVE_NO_SYMLINKS */;
        o2_res = syscall(SYS_openat2, AT_FDCWD, s_lnk, &how, sizeof(how));
        T(o2_res == -1 && errno == ELOOP, "openat2 RESOLVE_NO_SYMLINKS with symlink fails with ELOOP");

        /* openat2 normal file with RESOLVE_BENEATH -> succeeds */
        how.resolve = 0;
        int o2_fd = (int)syscall(SYS_openat2, AT_FDCWD, s_tgt, &how, sizeof(how));
        T(o2_fd >= 0, "openat2 opens normal file");
        if (o2_fd >= 0) close(o2_fd);

        unlink(s_lnk);
        unlink(s_tgt);
        rmdir(o2_base);
    }

    printf("-- epoll_pwait2 fidelity --\n");
    int ep = (int)syscall(SYS_epoll_create1, 0);
    T(ep >= 0, "epoll_create1");
    if (ep >= 0) {
        struct timespec bad_ts = { .tv_sec = 0, .tv_nsec = 1000000000L };
        long ep2_res = syscall(SYS_epoll_pwait2, ep, NULL, 0, &bad_ts, NULL, 0);
        T(ep2_res == -1 && errno == EINVAL, "epoll_pwait2 invalid tv_nsec fails with EINVAL");

        struct timespec zero_ts = { .tv_sec = 0, .tv_nsec = 0 };
        struct { unsigned int events; unsigned long long data; } ev = {0};
        ep2_res = syscall(SYS_epoll_pwait2, ep, &ev, 1, &zero_ts, NULL, 0);
        T(ep2_res == 0, "epoll_pwait2 zero timeout returns 0");
        close(ep);
    }

    printf("-- container namespaces / isolation --\n");
    long un_res = syscall(SYS_unshare, 0x04000000 /* CLONE_NEWUTS */);
    T(un_res == 0, "unshare(CLONE_NEWUTS) succeeds");
    if (un_res == 0) {
        int sh_res = sethostname("container-node", 14);
        T(sh_res == 0, "sethostname in unshared UTS namespace");
        struct utsname uts_sub;
        uname(&uts_sub);
        T(strcmp(uts_sub.nodename, "container-node") == 0, "uname reflects container nodename");
    }

    long pr_res = syscall(SYS_pivot_root, "/nonexistent/new_root", "/nonexistent/put_old");
    T(pr_res == -1 && errno == ENOENT, "pivot_root nonexistent new_root fails with ENOENT");

    char ns_link[128];
    ssize_t ns_len = readlink("/proc/self/ns/uts", ns_link, sizeof(ns_link) - 1);
    T(ns_len > 0, "readlink /proc/self/ns/uts returns link");
    if (ns_len > 0) {
        ns_link[ns_len] = '\0';
        T(strncmp(ns_link, "uts:[", 5) == 0, "/proc/self/ns/uts target has uts:[...] format");
    }

    printf("\n== probe complete: %d passed, %d failed ==\n", pass, fail);
    fflush(NULL);
    return fail ? 1 : 0;
}
