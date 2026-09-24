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
    T(ts_ns(&s1) >= target && ts_ns(&s1) - target < 30000000LL, "absolute sleep ends at the deadline");
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
    unlink(orig); rmdir(base);

    printf("\n== probe complete: %d passed, %d failed ==\n", pass, fail);
    fflush(NULL);
    return fail ? 1 : 0;
}
