/* ============================================================================
 * AzamiOS Userspace — System Init Daemon (init.elf - PID 1)
 * File: user/apps/init/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/prctl.h>
#include <sys/auxv.h>
#include <sys/random.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/inotify.h>
#include <sys/xattr.h>
#include <sysexits.h>
#include <az/ipc.h>



static int g_tests_passed = 0;
static int g_tests_total = 0;

static volatile int g_sig_hits = 0;
static volatile int g_last_sig = 0;
static void sig_test_handler(int signo) { g_sig_hits++; g_last_sig = signo; }

#define TEST_ASSERT(expr, desc) do { \
    g_tests_total++; \
    if (expr) { \
        printf("  [PASS] %s\n", desc); \
        g_tests_passed++; \
    } else { \
        printf("  [FAIL] %s (errno=%d)\n", desc, errno); \
    } \
} while (0)

static void run_posix_verification_suite(void)
{
    puts("-------------------------------------------------------------------------------");
    puts("             Running POSIX Conformance & System Self-Test Suite");
    puts("-------------------------------------------------------------------------------");

    /* 1. Process credentials and groups */
    uid_t ruid = 999, euid = 999, suid = 999;
    gid_t rgid = 999, egid = 999, sgid = 999;
    int r_uid = getresuid(&ruid, &euid, &suid);
    int r_gid = getresgid(&rgid, &egid, &sgid);
    TEST_ASSERT(r_uid == 0 && r_gid == 0 && ruid == 0 && rgid == 0, "getresuid / getresgid credentials retrieval");

    gid_t groups[16];
    int n_groups = getgroups(16, groups);
    TEST_ASSERT(n_groups >= 1 && groups[0] == 0, "getgroups returns active group list");

    pid_t pgid = getpgid(0);
    pid_t sid = getsid(0);
    TEST_ASSERT(pgid > 0 && sid > 0, "getpgid and getsid query session/process group");

    /* 2. High-resolution clocks and timers */
    struct timespec ts_res, ts_real, ts_mono;
    int r_clk1 = clock_getres(CLOCK_REALTIME, &ts_res);
    int r_clk2 = clock_gettime(CLOCK_REALTIME, &ts_real);
    int r_clk3 = clock_gettime(CLOCK_MONOTONIC, &ts_mono);
    TEST_ASSERT(r_clk1 == 0 && r_clk2 == 0 && r_clk3 == 0 && ts_real.tv_sec > 0, "clock_getres and clock_gettime (REALTIME & MONOTONIC)");

    struct timespec ts_req = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1 ms */
    int r_sleep = clock_nanosleep(CLOCK_REALTIME, 0, &ts_req, NULL);
    TEST_ASSERT(r_sleep == 0, "clock_nanosleep high-resolution sleep");

    /* 3. CPU scheduling and affinity */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    int r_aff_get = sched_getaffinity(0, sizeof(cpuset), &cpuset);
    TEST_ASSERT(r_aff_get >= 0 && CPU_ISSET(0, &cpuset), "sched_getaffinity / CPU_ISSET bitmap inspection");

    struct timespec rr_interval;
    int r_rr = sched_rr_get_interval(0, &rr_interval);
    TEST_ASSERT(r_rr == 0 && rr_interval.tv_nsec > 0, "sched_rr_get_interval timeslice query");

    int r_yield = sched_yield();
    TEST_ASSERT(r_yield == 0, "sched_yield execution");

    /* 4. Memory management (mmap, mprotect, munmap, aligned allocations) */
    void *m = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    TEST_ASSERT(m != MAP_FAILED && m != NULL, "mmap anonymous page allocation");
    if (m != MAP_FAILED) {
        memset(m, 0xAA, 8192);
        int r_mprot = mprotect(m, 8192, PROT_READ);
        TEST_ASSERT(r_mprot == 0, "mprotect permission update");
        int r_msync = msync(m, 8192, MS_SYNC);
        TEST_ASSERT(r_msync == 0, "msync memory synchronization");
        int r_mun = munmap(m, 8192);
        TEST_ASSERT(r_mun == 0, "munmap page release");
    }

    void *aligned_ptr = NULL;
    int r_align = posix_memalign(&aligned_ptr, 64, 256);
    TEST_ASSERT(r_align == 0 && aligned_ptr != NULL && (((uintptr_t)aligned_ptr & 63) == 0), "posix_memalign 64-byte alignment");
    if (aligned_ptr) free(aligned_ptr);

    /* 5. Auxv, prctl, and random generator */
    unsigned long pagesz = getauxval(AT_PAGESZ);
    unsigned long clktck = getauxval(AT_CLKTCK);
    TEST_ASSERT(pagesz == 4096 && clktck == 100, "getauxval (AT_PAGESZ=4096, AT_CLKTCK=100)");

    char proc_name[16] = { 0 };
    int r_pr1 = prctl(PR_GET_NAME, (unsigned long)proc_name, 0, 0, 0);
    TEST_ASSERT(r_pr1 == 0 && strlen(proc_name) > 0, "prctl PR_GET_NAME retrieval");

    unsigned char rand_buf[32];
    ssize_t n_rand = getrandom(rand_buf, sizeof(rand_buf), 0);
    TEST_ASSERT(n_rand == sizeof(rand_buf), "getrandom entropy stream generation");

    /* 6. File locking & synchronization */
    int test_fd = open("/tmp/posix_test.tmp", O_RDWR | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT(test_fd >= 0, "open(/tmp/posix_test.tmp) with O_CREAT");
    if (test_fd >= 0) {
        const char *tdata = "AzamiOS POSIX Test\n";
        ssize_t nw = write(test_fd, tdata, strlen(tdata));
        TEST_ASSERT(nw == (ssize_t)strlen(tdata), "write data to file");

        int r_sync = fsync(test_fd);
        int r_dsync = fdatasync(test_fd);
        int r_sfs = syncfs(test_fd);
        TEST_ASSERT(r_sync == 0 && r_dsync == 0 && r_sfs == 0, "fsync, fdatasync, and syncfs");

        int r_flk = flock(test_fd, LOCK_SH);
        TEST_ASSERT(r_flk == 0, "flock file advisory locking");

        struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
        int r_fcntl_lk = fcntl(test_fd, F_GETLK, &fl);
        TEST_ASSERT(r_fcntl_lk == 0, "fcntl POSIX record locking (F_GETLK)");

        close(test_fd);

        /* O_APPEND always writes at end-of-file regardless of seek. */
        int ap_fd = open("/tmp/posix_test.tmp", O_WRONLY | O_APPEND);
        if (ap_fd >= 0) {
            lseek(ap_fd, 0, SEEK_SET);
            write(ap_fd, "XYZ", 3);
            close(ap_fd);
        }
        struct stat ap_st;
        int r_ap = stat("/tmp/posix_test.tmp", &ap_st);
        TEST_ASSERT(r_ap == 0 && ap_st.st_size == (off_t)(strlen(tdata) + 3),
                    "O_APPEND writes at end-of-file after SEEK_SET");

        unlink("/tmp/posix_test.tmp");
    }

    /* 6b. Symbolic links: lstat / readlink must act on the link, not the target */
    {
        const char *lp = "/tmp/posix_test.link";
        const char *tgt = "/tmp/posix_test.target";
        unlink(lp); unlink(tgt);
        int tf = open(tgt, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (tf >= 0) { write(tf, "abcd", 4); close(tf); }

        int r_sl = symlink(tgt, lp);
        TEST_ASSERT(r_sl == 0, "symlink() creates a symbolic link");

        struct stat ls, ss;
        int r_ls = lstat(lp, &ls);
        int r_ss = stat(lp, &ss);
        TEST_ASSERT(r_ls == 0 && S_ISLNK(ls.st_mode), "lstat() reports the link itself (S_ISLNK)");
        TEST_ASSERT(r_ss == 0 && S_ISREG(ss.st_mode) && ss.st_size == 4,
                    "stat() follows the link to the target");

        char lbuf[64];
        ssize_t r_rl = readlink(lp, lbuf, sizeof(lbuf) - 1);
        TEST_ASSERT(r_rl == (ssize_t)strlen(tgt) &&
                    (lbuf[r_rl] = 0, strcmp(lbuf, tgt) == 0),
                    "readlink() returns the link target path");

        int nf = open(lp, O_RDONLY | O_NOFOLLOW);
        TEST_ASSERT(nf < 0, "open(O_NOFOLLOW) on a symlink fails");
        if (nf >= 0) close(nf);

        /* lchown() acts on the link, not the target. */
        int r_lch = lchown(lp, 0, 0);
        struct stat lch_st;
        int r_lst2 = lstat(lp, &lch_st);
        TEST_ASSERT(r_lch == 0 && r_lst2 == 0 && S_ISLNK(lch_st.st_mode),
                    "lchown() operates on the symlink itself");

        unlink(lp); unlink(tgt);
    }

    /* 6c. creat(), positional vectored I/O, and mlock() */
    {
        int cfd = creat("/tmp/posix_pv.tmp", 0644);
        TEST_ASSERT(cfd >= 0, "creat() creates a new writable file");
        if (cfd >= 0) {
            char b0[5] = "HELLO", b1[5] = "world";
            struct iovec wv[2] = { { b0, 5 }, { b1, 5 } };
            ssize_t nw = pwritev(cfd, wv, 2, 0);
            TEST_ASSERT(nw == 10, "pwritev() writes all iovec segments at offset");

            char r0[4], r1[6];
            struct iovec rv[2] = { { r0, 4 }, { r1, 6 } };
            ssize_t nr = preadv(cfd, rv, 2, 1);
            TEST_ASSERT(nr == 9 && memcmp(r0, "ELLO", 4) == 0 && memcmp(r1, "world", 5) == 0,
                        "preadv() reads scattered from an explicit offset");
            close(cfd);
        }
        unlink("/tmp/posix_pv.tmp");

        void *lk = malloc(8192);
        int r_ml = mlock(lk, 8192);
        int r_mu = munlock(lk, 8192);
        TEST_ASSERT(r_ml == 0 && r_mu == 0, "mlock / munlock succeed (no swap device)");
        free(lk);
    }

    /* 6d. Real userspace signal delivery */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = sig_test_handler;
        int r_sa = sigaction(SIGUSR1, &sa, NULL);

        g_sig_hits = 0; g_last_sig = 0;
        raise(SIGUSR1);
        raise(SIGUSR1);
        TEST_ASSERT(r_sa == 0 && g_sig_hits == 2 && g_last_sig == SIGUSR1,
                    "sigaction handler runs and control resumes (x2)");

        sigset_t m;
        sigemptyset(&m);
        sigaddset(&m, SIGUSR1);
        sigprocmask(SIG_BLOCK, &m, NULL);
        g_sig_hits = 0;
        raise(SIGUSR1);
        int while_blocked = g_sig_hits;
        sigprocmask(SIG_UNBLOCK, &m, NULL);
        TEST_ASSERT(while_blocked == 0 && g_sig_hits == 1,
                    "signal held pending while blocked, delivered on unblock");

        signal(SIGUSR1, SIG_DFL);
    }

    /* 7. Sockets and IPC */
    int sv[2];
    int r_sp = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    TEST_ASSERT(r_sp == 0 && sv[0] >= 0 && sv[1] >= 0, "socketpair(AF_UNIX, SOCK_STREAM)");
    if (r_sp == 0) {
        const char *msg = "ping-pong";
        write(sv[0], msg, strlen(msg));
        char rbuf[32] = { 0 };
        ssize_t nr = read(sv[1], rbuf, sizeof(rbuf) - 1);
        TEST_ASSERT(nr == (ssize_t)strlen(msg) && strcmp(rbuf, msg) == 0, "socketpair bi-directional message exchange");
        close(sv[0]);
        close(sv[1]);
    }

    /* 8. POSIX String & Math & Pseudo-Random Algorithms */
    int bcmp_res = timingsafe_bcmp("hello", "hello", 5);
    int bcmp_diff = timingsafe_bcmp("hello", "world", 5);
    TEST_ASSERT(bcmp_res == 0 && bcmp_diff != 0, "timingsafe_bcmp constant-time comparison");

    const char *v1 = "1.2.3";
    const char *v2 = "1.2.3";
    TEST_ASSERT(strverscmp(v1, v2) == 0, "strverscmp version comparison");

    double d48 = drand48();
    long l48 = lrand48();
    TEST_ASSERT(d48 >= 0.0 && d48 < 1.0 && l48 >= 0, "drand48 and lrand48 random number series");

    /* 9. Network Stack & DNS Resolver Self-Tests */
    struct hostent *he = gethostbyname("localhost");
    TEST_ASSERT(he && he->h_addr_list && he->h_addr_list[0] &&
                memcmp(he->h_addr_list[0], "\x7f\x00\x00\x01", 4) == 0, "gethostbyname(localhost) -> 127.0.0.1");

    struct protoent *pe = getprotobyname("tcp");
    TEST_ASSERT(pe && pe->p_proto == 6, "getprotobyname(tcp) -> IPPROTO_TCP 6");

    struct servent *se = getservbyname("http", "tcp");
    TEST_ASSERT(se && ntohs((unsigned short)se->s_port) == 80, "getservbyname(http, tcp) -> port 80");

    in_addr_t net_val = inet_network("127.0.0.1");
    TEST_ASSERT(net_val == 0x7f000001, "inet_network(127.0.0.1)");

    /* UDP Loopback Datagram Exchange */
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    TEST_ASSERT(udp_fd >= 0, "socket(AF_INET, SOCK_DGRAM)");
    if (udp_fd >= 0) {
        struct sockaddr_in u_addr;
        memset(&u_addr, 0, sizeof(u_addr));
        u_addr.sin_family = AF_INET;
        u_addr.sin_port = htons(9876);
        u_addr.sin_addr.s_addr = htonl(0x7f000001);
        int r_ubind = bind(udp_fd, (struct sockaddr *)&u_addr, sizeof(u_addr));
        TEST_ASSERT(r_ubind == 0, "bind(UDP, 127.0.0.1:9876)");

        const char *udp_msg = "HelloAzamiUDP";
        ssize_t n_usend = sendto(udp_fd, udp_msg, strlen(udp_msg), 0, (struct sockaddr *)&u_addr, sizeof(u_addr));
        TEST_ASSERT(n_usend == (ssize_t)strlen(udp_msg), "sendto(UDP datagram)");

        char u_recvbuf[32] = { 0 };
        ssize_t n_urecv = recvfrom(udp_fd, u_recvbuf, sizeof(u_recvbuf) - 1, 0, NULL, NULL);
        TEST_ASSERT(n_urecv == (ssize_t)strlen(udp_msg) && strcmp(u_recvbuf, udp_msg) == 0, "recvfrom(UDP datagram loopback)");

        close(udp_fd);
    }

    /* 10. Linux eventfd, timerfd & inotify subsystems */
    int efd = eventfd(0, 0);
    TEST_ASSERT(efd >= 0, "eventfd(0, 0) file descriptor allocation");
    if (efd >= 0) {
        uint64_t val = 42;
        ssize_t nw = write(efd, &val, sizeof(val));
        uint64_t rval = 0;
        ssize_t nr = read(efd, &rval, sizeof(rval));
        TEST_ASSERT(nw == sizeof(val) && nr == sizeof(rval) && rval == 42, "eventfd write & read event counter");
        close(efd);
    }

    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    TEST_ASSERT(tfd >= 0, "timerfd_create(CLOCK_MONOTONIC)");
    if (tfd >= 0) {
        struct itimerspec its = {
            .it_interval = { 0, 0 },
            .it_value = { 0, 1000000 } /* 1 ms */
        };
        int r_tset = timerfd_settime(tfd, 0, &its, NULL);
        TEST_ASSERT(r_tset == 0, "timerfd_settime configuration");
        close(tfd);
    }

    int ifd = inotify_init();
    TEST_ASSERT(ifd >= 0, "inotify_init file descriptor creation");
    if (ifd >= 0) {
        int iwd = inotify_add_watch(ifd, "/tmp", IN_ALL_EVENTS);
        TEST_ASSERT(iwd >= 0, "inotify_add_watch(/tmp, IN_ALL_EVENTS)");
        if (iwd >= 0) {
            char ibuf[256];
            ssize_t in_len = read(ifd, ibuf, sizeof(ibuf));
            TEST_ASSERT(in_len >= (ssize_t)sizeof(struct inotify_event), "inotify read filesystem event");
            inotify_rm_watch(ifd, iwd);
        }
        close(ifd);
    }

    /* 11. Linux Extended Attributes (xattr) */
    const char *xtest_file = "/tmp/xattr_test.tmp";
    int xfd = open(xtest_file, O_CREAT | O_RDWR, 0644);
    if (xfd >= 0) close(xfd);

    int r_xset = setxattr(xtest_file, "user.checksum", "abcdef123456", 12, 0);
    TEST_ASSERT(r_xset == 0, "setxattr(user.checksum)");

    char xval[64] = { 0 };
    ssize_t xlen = getxattr(xtest_file, "user.checksum", xval, sizeof(xval) - 1);
    TEST_ASSERT(xlen == 12 && strcmp(xval, "abcdef123456") == 0, "getxattr(user.checksum) verification");

    char xlist[128] = { 0 };
    ssize_t xlist_len = listxattr(xtest_file, xlist, sizeof(xlist));
    TEST_ASSERT(xlist_len > 0, "listxattr returns attribute list");

    int r_xrem = removexattr(xtest_file, "user.checksum");
    TEST_ASSERT(r_xrem == 0, "removexattr(user.checksum)");
    unlink(xtest_file);

    /* 12. Native GCC Toolchain Verification */
    int as_pid = fork();
    if (as_pid == 0) {
        char *const as_argv[] = {"/usr/bin/as", "--version", NULL};
        char *const as_envp[] = {"PATH=/bin:/usr/bin", NULL};
        execve("/usr/bin/as", as_argv, as_envp);
        exit(127);
    }
    int as_status = 0;
    waitpid(as_pid, &as_status, 0);
    printf("[INIT-AS] as --version returned status=0x%x (exit_code=%d)\n", as_status, (as_status >> 8) & 0xFF);
    TEST_ASSERT(as_status == 0, "GNU Assembler (as) native execution");

    int gcc_pid = fork();
    if (gcc_pid == 0) {
        int lfd = open("/tmp/gcc_log.txt", O_CREAT | O_WRONLY | O_TRUNC, 0666);
        if (lfd >= 0) {
            dup2(lfd, 1);
            dup2(lfd, 2);
            close(lfd);
        }

        char *const gcc_argv[] = {"/usr/bin/gcc", "-v", "-c", "/examples/hello.c", "-o", "/tmp/hello.o", NULL};

        char *const gcc_envp[] = {
            "PATH=/usr/bin:/bin:/usr/libexec/gcc/x86_64-elf/14.2.0",
            "TMPDIR=/tmp",
            "C_INCLUDE_PATH=/usr/include",
            NULL
        };
        execve("/usr/bin/gcc", gcc_argv, gcc_envp);
        exit(127);
    }
    int gcc_status = 0;
    waitpid(gcc_pid, &gcc_status, 0);
    printf("[INIT-GCC] gcc -c /examples/hello.c -> /tmp/hello.o returned status=0x%x (exit_code=%d)\n", gcc_status, (gcc_status >> 8) & 0xFF);

    int rfd = open("/tmp/gcc_log.txt", O_RDONLY);
    if (rfd >= 0) {
        printf("[GCC-LOG-START]\n");
        char lbuf[512];
        ssize_t n;
        while ((n = read(rfd, lbuf, sizeof(lbuf) - 1)) > 0) {
            lbuf[n] = '\0';
            printf("%s", lbuf);
        }
        printf("\n[GCC-LOG-END]\n");
        close(rfd);
    }
    TEST_ASSERT(gcc_status == 0, "GNU GCC 14.2.0 native C compilation (cc1 + as)");

    int ld_status = -1;
    if (gcc_status == 0) {
        int ld_pid = fork();
        if (ld_pid == 0) {
            char *const ld_argv[] = {"/usr/bin/ld", "-nostdlib", "/usr/lib/crt0.o", "/tmp/hello.o", "/usr/lib/libc.a", "-o", "/tmp/hello", NULL};
            char *const ld_envp[] = {"PATH=/bin:/usr/bin", NULL};
            execve("/usr/bin/ld", ld_argv, ld_envp);
            exit(127);
        }
        waitpid(ld_pid, &ld_status, 0);
        printf("[INIT-LD] ld /tmp/hello.o -> /tmp/hello returned status=0x%x (exit_code=%d)\n", ld_status, (ld_status >> 8) & 0xFF);
        TEST_ASSERT(ld_status == 0, "GNU Binutils (ld) native ELF linking");
    }

    if (ld_status == 0) {
        int run_pid = fork();
        if (run_pid == 0) {
            char *const hello_argv[] = {"/tmp/hello", "native_test", NULL};
            char *const hello_envp[] = {NULL};
            execve("/tmp/hello", hello_argv, hello_envp);
            exit(127);
        }
        int run_status = 0;
        waitpid(run_pid, &run_status, 0);
        printf("[INIT-HELLO] /tmp/hello execution returned status=0x%x (exit_code=%d)\n", run_status, (run_status >> 8) & 0xFF);
        TEST_ASSERT(run_status == 0, "Native compiled binary execution (/tmp/hello)");
    }


    printf("-------------------------------------------------------------------------------\n");
    printf("  POSIX & Network Verification Results: %d / %d Tests Passed (%.1f%%)\n",
           g_tests_passed, g_tests_total, ((double)g_tests_passed / (double)g_tests_total) * 100.0);
    printf("-------------------------------------------------------------------------------\n");
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    puts("===============================================================================");
    puts("             AzamiOS v7.0 — Modular Ring 3 Userspace (init.elf)");
    puts("===============================================================================");
    /* Run POSIX conformance and self-test verification suite */
    run_posix_verification_suite();

    /* Spawn Network DHCP Daemon */
    az_spawn("/sbin/dhcpcd.elf");

    puts("[init] Spawning session manager (sessiond.elf)...");

    /* ── Spawn Session Manager Daemon ────────────────────────────────────── */
    int pid = az_spawn("/sbin/sessiond.elf");
    if (pid < 0) {
        puts("[init] ERROR: az_spawn /sbin/sessiond.elf failed! Attempting direct fallback...");
        az_spawn("/sbin/azwm.elf");
        for (int i = 0; i < 100; i++) az_yield();
        az_spawn("/sbin/wallpaper.elf");
        az_spawn("/sbin/taskbar.elf");
    } else {
        puts("[init] sessiond.elf spawned successfully.");
    }

    /* ── PID 1 Idle & Zombie Reaper Loop ─────────────────────────────────── */
    for (;;) {
        sleep(1);
    }

    return 0;
}

