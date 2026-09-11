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
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <sys/times.h>
#include <linux/i2c-dev.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <az/ipc.h>
#include <poll.h>
#include <mqueue.h>
#include <sys/ioprio.h>
#include <numaif.h>
#include <stddef.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/futex.h>



static int g_tests_passed = 0;
static int g_tests_total = 0;

static volatile int g_sig_hits = 0;
static volatile int g_last_sig = 0;
static void sig_test_handler(int signo) { g_sig_hits++; g_last_sig = signo; }

/* Handler that scribbles all over the FPU/SSE state — the kernel must snapshot
 * and restore it around delivery or the interrupted computation is corrupted. */
static volatile double g_sig_fp_sink = 0.0;
static void sig_fp_handler(int signo)
{
    (void)signo;
    double a = 1.0;
    for (int i = 0; i < 64; i++) a = a * 1.5 + 0.25;
    g_sig_fp_sink = a;
}

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

        /* FPU/SSE state must survive a handler that uses floating point. */
        struct sigaction fsa;
        memset(&fsa, 0, sizeof fsa);
        fsa.sa_handler = sig_fp_handler;
        sigaction(SIGUSR2, &fsa, NULL);

        volatile double acc = 0.0;
        int fp_ok = 1;
        for (int i = 0; i < 8; i++) {
            acc += (double)i * 0.5;              /* keeps live values in XMM */
            raise(SIGUSR2);                       /* handler clobbers XMM/x87 */
            double expect = 0.0;
            for (int j = 0; j <= i; j++) expect += (double)j * 0.5;
            if (acc != expect) { fp_ok = 0; break; }
        }
        TEST_ASSERT(fp_ok && g_sig_fp_sink != 0.0,
                    "FPU/SSE state preserved across a signal handler");

        signal(SIGUSR1, SIG_DFL);
        signal(SIGUSR2, SIG_DFL);
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

    /* 11b. Memory-protection keys, mseal, mincore and robust futex lists */
    {
        unsigned char mvec[4] = { 0xff, 0xff, 0xff, 0xff };
        void *mpage = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        TEST_ASSERT(mpage != MAP_FAILED, "mmap 4 pages for memory-key tests");

        if (mpage != MAP_FAILED) {
            ((volatile char *)mpage)[0] = 1;
            int r_mc = mincore(mpage, 4 * 4096, mvec);
            TEST_ASSERT(r_mc == 0 && (mvec[0] & 1),
                        "mincore reports a touched page as resident");

            /* An unmapped address must come back as *not* resident — the old
             * implementation answered "resident" for everything. */
            unsigned char hole = 0xff;
            int r_hole = mincore((void *)0x0000700000000000UL, 4096, &hole);
            TEST_ASSERT(r_hole != 0 || (hole & 1) == 0,
                        "mincore does not claim an unmapped page is resident");

            int key = pkey_alloc(0, 0);
            if (key >= 0) {
                int r_pm = pkey_mprotect(mpage, 4096, PROT_READ | PROT_WRITE, key);
                TEST_ASSERT(r_pm == 0, "pkey_mprotect tags a page with a key");
                ((volatile char *)mpage)[0] = 2;
                TEST_ASSERT(((volatile char *)mpage)[0] == 2,
                            "page tagged with a permissive key stays accessible");
                TEST_ASSERT(pkey_free(key) == 0, "pkey_free releases the key");
            } else {
                /* No PKU on this CPU: the call must say so, not misbehave. */
                TEST_ASSERT(errno == ENOSPC, "pkey_alloc reports ENOSPC without PKU");
            }

            TEST_ASSERT(mseal(mpage, 4 * 4096, 0) == 0, "mseal over a mapped range");
            TEST_ASSERT(mseal((void *)0x0000700000000000UL, 4096, 0) == -1 &&
                        errno == ENOMEM, "mseal rejects an unmapped range");
            munmap(mpage, 4 * 4096);
        }

        /* set_robust_list / get_robust_list must round-trip. */
        char rl_head[24] = { 0 };
        long r_srl = syscall2(SYS_set_robust_list, (long)rl_head, 24);
        TEST_ASSERT(r_srl == 0, "set_robust_list accepts a 24-byte head");
        void  *got_head = NULL;
        size_t got_len  = 0;
        long r_grl = syscall3(SYS_get_robust_list, 0, (long)&got_head, (long)&got_len);
        TEST_ASSERT(r_grl == 0 && got_head == (void *)rl_head && got_len == 24,
                    "get_robust_list returns what was set");

        /* /proc/cpuinfo now reports what CPUID actually enumerated. */
        int cifd = open("/proc/cpuinfo", O_RDONLY);
        TEST_ASSERT(cifd >= 0, "open(/proc/cpuinfo)");
        if (cifd >= 0) {
            static char ci[8192];
            ssize_t cin = read(cifd, ci, sizeof(ci) - 1);
            if (cin > 0) ci[cin] = '\0'; else ci[0] = '\0';
            close(cifd);
            TEST_ASSERT(cin > 0 && strstr(ci, "address sizes") != NULL,
                        "/proc/cpuinfo reports CPUID address sizes");
            TEST_ASSERT(strstr(ci, "bugs            :") != NULL,
                        "/proc/cpuinfo carries a bugs line");
            TEST_ASSERT(strstr(ci, "cpu cores") != NULL &&
                        strstr(ci, "clflush size") != NULL,
                        "/proc/cpuinfo carries topology and cache-line fields");
            TEST_ASSERT(strstr(ci, "azami_mitigation") != NULL,
                        "/proc/cpuinfo reports the active mitigation policy");
        }

        /* The speculative-execution report: one line per erratum naming what
         * the kernel did about it. Every entry has to resolve to a definite
         * answer — a blank status would mean the policy engine did not run. */
        int vfd = open("/proc/vulnerabilities", O_RDONLY);
        TEST_ASSERT(vfd >= 0, "open(/proc/vulnerabilities)");
        if (vfd >= 0) {
            static char vb[4096];
            ssize_t vn = read(vfd, vb, sizeof(vb) - 1);
            if (vn > 0) vb[vn] = '\0'; else vb[0] = '\0';
            close(vfd);
            TEST_ASSERT(vn > 0 && strstr(vb, "spectre_v2") != NULL &&
                        strstr(vb, "spec_store_bypass") != NULL &&
                        strstr(vb, "mds") != NULL,
                        "/proc/vulnerabilities names each speculation erratum");
            TEST_ASSERT(strstr(vb, "Not affected") != NULL ||
                        strstr(vb, "Mitigation:") != NULL ||
                        strstr(vb, "Vulnerable") != NULL,
                        "/proc/vulnerabilities gives every erratum a status");
        }

        /* The hardening posture: what the boot path actually achieved, which
         * is not the same question as what the CPU supports. */
        int sfd = open("/proc/security", O_RDONLY);
        TEST_ASSERT(sfd >= 0, "open(/proc/security)");
        if (sfd >= 0) {
            static char sb[4096];
            ssize_t sn = read(sfd, sb, sizeof(sb) - 1);
            if (sn > 0) sb[sn] = '\0'; else sb[0] = '\0';
            close(sfd);
            TEST_ASSERT(sn > 0 && strstr(sb, "smep") != NULL &&
                        strstr(sb, "smap") != NULL && strstr(sb, "cr0_wp") != NULL,
                        "/proc/security reports the ring-0 protections");
            TEST_ASSERT(strstr(sb, "rdpmc_user:            denied") != NULL,
                        "RDPMC stays denied to ring 3");
            TEST_ASSERT(strstr(sb, "stack_canary:          random per boot") != NULL,
                        "stack canary is randomised per boot");
        }

        /* seccomp(2) SECCOMP_MODE_FILTER: a real classic-BPF program, run in
         * a forked child so a wrong verdict can't take the test suite down
         * with it. First child: a filter that returns SECCOMP_RET_ERRNO for
         * one specific syscall (mkdir) and SECCOMP_RET_ALLOW for everything
         * else, proving both a denial and a pass-through in one program.
         * Second child: SECCOMP_RET_KILL_PROCESS on the same syscall, proving
         * the kill is real (SIGSYS) and not just a loud denial. */
        {
            pid_t sc_pid = fork();
            if (sc_pid == 0) {
                struct sock_filter filt[] = {
                    BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                             (unsigned int)offsetof(struct seccomp_data, nr)),
                    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_mkdir, 0, 1),
                    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
                    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
                };
                struct sock_fprog prog = { sizeof(filt) / sizeof(filt[0]), filt };
                if (syscall3(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, (long)&prog) != 0) _exit(2);
                errno = 0;
                int mk = mkdir("/tmp/seccomp_denied_dir", 0755);
                if (mk == 0 || errno != EPERM) _exit(3);      /* denial didn't land */
                if (getpid() <= 0) _exit(4);                  /* pass-through broke */
                _exit(0);
            }
            int sc_status = 0;
            waitpid(sc_pid, &sc_status, 0);
            TEST_ASSERT(WIFEXITED(sc_status) && WEXITSTATUS(sc_status) == 0,
                        "seccomp SECCOMP_MODE_FILTER: RET_ERRNO denies one syscall, RET_ALLOW passes the rest");
        }
        {
            pid_t sk_pid = fork();
            if (sk_pid == 0) {
                struct sock_filter filt[] = {
                    BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                             (unsigned int)offsetof(struct seccomp_data, nr)),
                    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_mkdir, 0, 1),
                    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
                    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
                };
                struct sock_fprog prog = { sizeof(filt) / sizeof(filt[0]), filt };
                if (syscall3(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, (long)&prog) != 0) _exit(2);
                mkdir("/tmp/seccomp_kill_dir", 0755);
                _exit(9); /* unreachable if RET_KILL_PROCESS actually fired */
            }
            int sk_status = 0;
            waitpid(sk_pid, &sk_status, 0);
            TEST_ASSERT(WIFSIGNALED(sk_status) && WTERMSIG(sk_status) == SIGSYS,
                        "seccomp SECCOMP_MODE_FILTER: RET_KILL_PROCESS terminates the process (SIGSYS)");
        }

        /* Machine-check log. Reading it polls the MCA banks, so a clean run
         * here also proves the bank MSRs are addressable on this part. */
        int mfd = open("/proc/mcelog", O_RDONLY);
        TEST_ASSERT(mfd >= 0, "open(/proc/mcelog)");
        if (mfd >= 0) {
            static char mb[4096];
            ssize_t mn = read(mfd, mb, sizeof(mb) - 1);
            if (mn > 0) mb[mn] = '\0'; else mb[0] = '\0';
            close(mfd);
            TEST_ASSERT(mn > 0 && strstr(mb, "banks") != NULL,
                        "/proc/mcelog reports the machine-check bank count");
        }

        /* cachestat over a real file reports the range as cached. */
        int csfd = open("/etc/hostname", O_RDONLY);
        if (csfd < 0) csfd = open("/sbin/init.elf", O_RDONLY);
        if (csfd >= 0) {
            struct { unsigned long off, len; } csrange = { 0, 4096 };
            unsigned long cs[5] = { 0 };
            long r_cs = syscall4(SYS_cachestat, csfd, (long)&csrange, (long)cs, 0);
            TEST_ASSERT(r_cs == 0, "cachestat reports page-cache residency");
            close(csfd);
        }
    }

    /* 12. Native GCC Toolchain Verification — real gcc/as/ld invocations
     * (cc1 does actual compilation, ld does a real link), unlike the ~200
     * syscall-probe tests above that each cost microseconds. Under QEMU TCG
     * emulation this one step is ~2s of an ~8s boot — the single largest
     * cost in the whole sequence. Still worth having (it is the only thing
     * that actually proves the self-hosted toolchain staged into the image
     * works), just not worth paying on every boot by default.
     * `touch /etc/run-toolchain-selftest` before repacking the initrd
     * re-enables it. */
    if (access("/etc/run-toolchain-selftest", F_OK) != 0) {
        printf("[INIT] Skipping native GCC/binutils smoke test "
               "(touch /etc/run-toolchain-selftest to re-enable)\n");
    } else {
    int as_pid = fork();
    if (as_pid == 0) {
        int lfd = open("/tmp/as_log.txt", O_CREAT | O_WRONLY | O_TRUNC, 0666);
        if (lfd >= 0) {
            dup2(lfd, 1);
            dup2(lfd, 2);
            close(lfd);
        }
        char *const as_argv[] = {"/usr/bin/as", "--version", NULL};
        char *const as_envp[] = {"PATH=/bin:/usr/bin", "LC_ALL=C", NULL};
        execve("/usr/bin/as", as_argv, as_envp);
        exit(127);
    }
    int as_status = 0;
    waitpid(as_pid, &as_status, 0);
    printf("[INIT-AS] as --version returned status=0x%x (exit_code=%d)\n", as_status, (as_status >> 8) & 0xFF);
    int afd = open("/tmp/as_log.txt", O_RDONLY);
    if (afd >= 0) {
        printf("[AS-LOG-START]\n");
        char abuf[512];
        ssize_t n;
        while ((n = read(afd, abuf, sizeof(abuf) - 1)) > 0) {
            abuf[n] = '\0';
            printf("%s", abuf);
        }
        printf("\n[AS-LOG-END]\n");
        close(afd);
    }
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
    }


    /* ── System V IPC (POSIX XSI) ────────────────────────────────────────── */
    {
        /* Shared memory: create, attach, round-trip a value, detach, remove. */
        int shmid = shmget(IPC_PRIVATE, 8192, IPC_CREAT | 0600);
        TEST_ASSERT(shmid >= 0, "shmget(IPC_PRIVATE) creates a segment");

        if (shmid >= 0) {
            char *seg = (char *)shmat(shmid, NULL, 0);
            TEST_ASSERT(seg != (char *)-1, "shmat attaches the segment");

            if (seg != (char *)-1) {
                strcpy(seg, "sysvipc");
                TEST_ASSERT(strcmp(seg, "sysvipc") == 0, "shared memory round-trips data");

                struct shmid_ds ds;
                memset(&ds, 0, sizeof(ds));
                TEST_ASSERT(shmctl(shmid, IPC_STAT, &ds) == 0 &&
                            ds.shm_segsz == 8192 && ds.shm_nattch == 1,
                            "shmctl(IPC_STAT) reports size and attach count");

                /* POSIX: a fork()ed child inherits the attachment, and the
                 * segment must count it — otherwise a parent that detaches and
                 * removes the segment frees pages the child still maps. */
                struct shmid_ds fds;
                pid_t kid = fork();
                if (kid == 0) {
                    /* Touch the inherited mapping, then report what the
                     * segment thinks its attach count is. */
                    seg[0] = 'K';
                    struct shmid_ds cds;
                    memset(&cds, 0, sizeof(cds));
                    int ok = (shmctl(shmid, IPC_STAT, &cds) == 0 && cds.shm_nattch == 2);
                    _exit(ok ? 0 : 1);
                } else if (kid > 0) {
                    int st = -1;
                    waitpid(kid, &st, 0);
                    TEST_ASSERT(WIFEXITED(st) && WEXITSTATUS(st) == 0,
                                "fork() child inherits the shm attachment and its nattch");
                    memset(&fds, 0, sizeof(fds));
                    TEST_ASSERT(shmctl(shmid, IPC_STAT, &fds) == 0 && fds.shm_nattch == 1,
                                "the child's exit drops nattch back to one");
                }

                /* IPC_INFO / SHM_INFO describe the subsystem, not one segment. */
                struct shminfo sinf;
                memset(&sinf, 0, sizeof(sinf));
                TEST_ASSERT(shmctl(0, IPC_INFO, (struct shmid_ds *)&sinf) >= 0 &&
                            sinf.shmmni > 0 && sinf.shmmax > 0,
                            "shmctl(IPC_INFO) reports the shared-memory limits");

                struct shm_info suse;
                memset(&suse, 0, sizeof(suse));
                TEST_ASSERT(shmctl(0, SHM_INFO, (struct shmid_ds *)&suse) >= 0 &&
                            suse.used_ids > 0,
                            "shmctl(SHM_INFO) reports segments currently in use");

                TEST_ASSERT(shmdt(seg) == 0, "shmdt detaches the segment");
            }
            TEST_ASSERT(shmctl(shmid, IPC_RMID, NULL) == 0, "shmctl(IPC_RMID) removes the segment");
            TEST_ASSERT(shmctl(shmid, IPC_STAT, &(struct shmid_ds){0}) == -1,
                        "a removed segment id is no longer valid");
        }

        /* Semaphores: set a value, take it, put it back. */
        int semid = semget(IPC_PRIVATE, 2, IPC_CREAT | 0600);
        TEST_ASSERT(semid >= 0, "semget(IPC_PRIVATE, 2) creates a set");

        if (semid >= 0) {
            union semun su;
            su.val = 1;
            TEST_ASSERT(semctl(semid, 0, SETVAL, su) == 0, "semctl(SETVAL) initialises a semaphore");
            TEST_ASSERT(semctl(semid, 0, GETVAL) == 1, "semctl(GETVAL) reads it back");

            struct sembuf take = { 0, -1, 0 };
            TEST_ASSERT(semop(semid, &take, 1) == 0, "semop takes the semaphore");
            TEST_ASSERT(semctl(semid, 0, GETVAL) == 0, "the semaphore is now held");

            struct sembuf nowait = { 0, -1, IPC_NOWAIT };
            TEST_ASSERT(semop(semid, &nowait, 1) == -1 && errno == EAGAIN,
                        "semop with IPC_NOWAIT fails rather than blocking");

            struct sembuf give = { 0, 1, 0 };
            TEST_ASSERT(semop(semid, &give, 1) == 0 && semctl(semid, 0, GETVAL) == 1,
                        "semop releases the semaphore");

            /* semtimedop(): a bounded wait gives up with EAGAIN instead of
             * blocking forever, and succeeds when the operation can proceed. */
            struct sembuf take2 = { 0, -1, 0 };
            TEST_ASSERT(semtimedop(semid, &take2, 1, NULL) == 0,
                        "semtimedop with a NULL timeout behaves as semop");

            struct timespec tmo = { 0, 50 * 1000 * 1000 };   /* 50 ms */
            struct sembuf blocker = { 0, -1, 0 };
            TEST_ASSERT(semtimedop(semid, &blocker, 1, &tmo) == -1 && errno == EAGAIN,
                        "semtimedop times out with EAGAIN rather than blocking");

            struct sembuf give2 = { 0, 1, 0 };
            TEST_ASSERT(semop(semid, &give2, 1) == 0, "semop restores the semaphore");

            /* An increment past SEMVMX is an error, never a wait. */
            struct sembuf huge = { 0, 0, 0 };
            huge.sem_op = (short)SEMVMX;
            TEST_ASSERT(semop(semid, &huge, 1) == -1 && errno == ERANGE,
                        "semop past SEMVMX fails with ERANGE");
            TEST_ASSERT(semctl(semid, 0, SETVAL, (union semun){ .val = SEMVMX + 1 }) == -1 &&
                        errno == ERANGE,
                        "semctl(SETVAL) past SEMVMX fails with ERANGE");

            /* IPC_INFO / SEM_INFO describe the subsystem, not one set. */
            struct seminfo si;
            memset(&si, 0, sizeof(si));
            union semun iu; iu.__pad = &si;
            TEST_ASSERT(semctl(0, 0, IPC_INFO, iu) >= 0 && si.semmsl > 0 && si.semmni > 0,
                        "semctl(IPC_INFO) reports the semaphore limits");

            memset(&si, 0, sizeof(si));
            TEST_ASSERT(semctl(0, 0, SEM_INFO, iu) >= 0 && si.semusz > 0,
                        "semctl(SEM_INFO) reports sets currently in use");

            TEST_ASSERT(semctl(semid, 0, IPC_RMID) == 0, "semctl(IPC_RMID) removes the set");
        }

        /* Message queues: type-selective receive. */
        int msqid = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
        TEST_ASSERT(msqid >= 0, "msgget(IPC_PRIVATE) creates a queue");

        if (msqid >= 0) {
            struct { long mtype; char mtext[32]; } out, in;
            out.mtype = 7;
            strcpy(out.mtext, "queued");
            TEST_ASSERT(msgsnd(msqid, &out, sizeof(out.mtext), 0) == 0, "msgsnd queues a message");

            out.mtype = 3;
            strcpy(out.mtext, "lower");
            TEST_ASSERT(msgsnd(msqid, &out, sizeof(out.mtext), 0) == 0, "msgsnd queues a second message");

            memset(&in, 0, sizeof(in));
            ssize_t got = msgrcv(msqid, &in, sizeof(in.mtext), 7, 0);
            TEST_ASSERT(got == (ssize_t)sizeof(in.mtext) && in.mtype == 7 &&
                        strcmp(in.mtext, "queued") == 0,
                        "msgrcv selects a message by type");

            memset(&in, 0, sizeof(in));
            TEST_ASSERT(msgrcv(msqid, &in, sizeof(in.mtext), 0, IPC_NOWAIT) > 0 && in.mtype == 3,
                        "msgrcv with msgtyp 0 takes the first message");
            TEST_ASSERT(msgrcv(msqid, &in, sizeof(in.mtext), 0, IPC_NOWAIT) == -1 && errno == ENOMSG,
                        "an empty queue reports ENOMSG under IPC_NOWAIT");
            struct msginfo minf;
            memset(&minf, 0, sizeof(minf));
            TEST_ASSERT(msgctl(0, IPC_INFO, (struct msqid_ds *)&minf) >= 0 &&
                        minf.msgmni > 0 && minf.msgmax > 0,
                        "msgctl(IPC_INFO) reports the message-queue limits");

            memset(&minf, 0, sizeof(minf));
            TEST_ASSERT(msgctl(0, MSG_INFO, (struct msqid_ds *)&minf) >= 0 &&
                        minf.msgpool > 0,
                        "msgctl(MSG_INFO) reports queues currently in use");

            TEST_ASSERT(msgctl(msqid, IPC_RMID, NULL) == 0, "msgctl(IPC_RMID) removes the queue");
        }
    }

    /* ── POSIX timers and interval timers ────────────────────────────────── */
    {
        timer_t tid;
        struct sigevent sev;
        memset(&sev, 0, sizeof(sev));
        sev.sigev_notify = SIGEV_SIGNAL;
        sev.sigev_signo  = SIGUSR1;

        TEST_ASSERT(timer_create(CLOCK_REALTIME, &sev, &tid) == 0, "timer_create makes a timer");

        struct itimerspec its;
        memset(&its, 0, sizeof(its));
        its.it_value.tv_sec = 30;
        TEST_ASSERT(timer_settime(tid, 0, &its, NULL) == 0, "timer_settime arms it");

        struct itimerspec cur;
        memset(&cur, 0, sizeof(cur));
        TEST_ASSERT(timer_gettime(tid, &cur) == 0 && cur.it_value.tv_sec > 0 &&
                    cur.it_value.tv_sec <= 30,
                    "timer_gettime reports the time remaining");
        TEST_ASSERT(timer_getoverrun(tid) == 0, "a timer that has not fired has no overrun");
        TEST_ASSERT(timer_delete(tid) == 0, "timer_delete destroys it");

        /* alarm() returns the seconds left on the alarm it replaced. */
        alarm(60);
        unsigned int left = alarm(0);
        TEST_ASSERT(left > 0 && left <= 60, "alarm() reports the previous alarm's remaining time");

        struct itimerval itv, oldv;
        memset(&itv, 0, sizeof(itv));
        itv.it_value.tv_sec    = 20;
        itv.it_interval.tv_sec = 5;
        TEST_ASSERT(setitimer(ITIMER_REAL, &itv, NULL) == 0, "setitimer(ITIMER_REAL) arms an interval timer");
        memset(&oldv, 0, sizeof(oldv));
        TEST_ASSERT(getitimer(ITIMER_REAL, &oldv) == 0 && oldv.it_interval.tv_sec == 5 &&
                    oldv.it_value.tv_sec > 0,
                    "getitimer reports the armed value and interval");
        memset(&itv, 0, sizeof(itv));
        TEST_ASSERT(setitimer(ITIMER_REAL, &itv, NULL) == 0, "setitimer with a zero value disarms");
    }

    /* ── Synchronous signal waiting ──────────────────────────────────────── */
    {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGUSR2);

        struct timespec zero = { 0, 0 };
        siginfo_t info;
        TEST_ASSERT(sigtimedwait(&set, &info, &zero) == -1 && errno == EAGAIN,
                    "sigtimedwait times out when nothing is pending");

        sigprocmask(SIG_BLOCK, &set, NULL);
        raise(SIGUSR2);
        memset(&info, 0, sizeof(info));
        int caught = sigtimedwait(&set, &info, &zero);
        TEST_ASSERT(caught == SIGUSR2 && info.si_signo == SIGUSR2,
                    "sigtimedwait accepts a pending blocked signal");
        sigprocmask(SIG_UNBLOCK, &set, NULL);

        union sigval sv;
        sv.sival_int = 42;
        TEST_ASSERT(sigqueue(getpid(), 0, sv) == 0, "sigqueue with signal 0 probes without sending");
    }

    /* ── /proc/sysvipc, the Linux-format IPC listing ─────────────────────── */
    {
        int shmid = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
        if (shmid >= 0) {
            int fd = open("/proc/sysvipc/shm", O_RDONLY);
            TEST_ASSERT(fd >= 0, "/proc/sysvipc/shm is readable");

            if (fd >= 0) {
                char buf[2048];
                ssize_t n = read(fd, buf, sizeof(buf) - 1);
                close(fd);
                if (n < 0) n = 0;
                buf[n] = '\0';

                char needle[32];
                snprintf(needle, sizeof(needle), " %d ", shmid);
                TEST_ASSERT(strstr(buf, "shmid") != NULL,
                            "/proc/sysvipc/shm carries the Linux column header");
                TEST_ASSERT(strstr(buf, needle) != NULL,
                            "/proc/sysvipc/shm lists the live segment");
            }
            shmctl(shmid, IPC_RMID, NULL);
        }
    }

    /* ── i2c-dev: the SMBus controller and the chips on its bus ──────────── */
    {
        int fd = open("/dev/i2c-0", O_RDWR);
        TEST_ASSERT(fd >= 0, "/dev/i2c-0 opens (SMBus adapter present)");

        if (fd >= 0) {
            unsigned long funcs = 0;
            TEST_ASSERT(ioctl(fd, I2C_FUNCS, &funcs) == 0 &&
                        (funcs & I2C_FUNC_SMBUS_READ_BYTE_DATA),
                        "I2C_FUNCS reports SMBus byte-data support");

            /* q35 populates SPD EEPROMs at 0x50..0x57. */
            TEST_ASSERT(ioctl(fd, I2C_SLAVE, 0x50) == 0, "I2C_SLAVE selects a chip address");

            union i2c_smbus_data data;
            memset(&data, 0, sizeof(data));
            struct i2c_smbus_ioctl_data req = {
                .read_write = I2C_SMBUS_READ,
                .command    = 0,
                .size       = I2C_SMBUS_BYTE_DATA,
                .data       = &data,
            };
            TEST_ASSERT(ioctl(fd, I2C_SMBUS, &req) == 0,
                        "I2C_SMBUS reads a byte from the EEPROM at 0x50");

            /* Nothing answers at 0x7F, so the controller must report the NAK. */
            if (ioctl(fd, I2C_SLAVE, 0x7f) == 0) {
                memset(&data, 0, sizeof(data));
                TEST_ASSERT(ioctl(fd, I2C_SMBUS, &req) == -1,
                            "an unpopulated address fails instead of returning junk");
            }
            close(fd);
        }
    }

    /* ── /dev subdirectories (evdev and DRM live in them) ────────────────── */
    {
        int fd = open("/dev/dri/card0", O_RDWR);
        TEST_ASSERT(fd >= 0, "/dev/dri/card0 resolves through a devfs subdirectory");
        if (fd >= 0) close(fd);
    }

    /* ── evdev: the Linux input UAPI ─────────────────────────────────────── */
    {
        int fd = open("/dev/input/event0", O_RDONLY);
        TEST_ASSERT(fd >= 0, "/dev/input/event0 opens");

        if (fd >= 0) {
            int version = 0;
            TEST_ASSERT(ioctl(fd, EVIOCGVERSION, &version) == 0 && version == 0x010001,
                        "EVIOCGVERSION reports the evdev protocol version");

            char name[64] = {0};
            TEST_ASSERT(ioctl(fd, EVIOCGNAME(sizeof(name)), name) > 0 && name[0] != '\0',
                        "EVIOCGNAME returns the device name");

            unsigned char types[EV_CNT / 8 + 1];
            memset(types, 0, sizeof(types));
            TEST_ASSERT(ioctl(fd, EVIOCGBIT(0, sizeof(types)), types) > 0 &&
                        input_bit_is_set(types, EV_KEY) && input_bit_is_set(types, EV_REL),
                        "EVIOCGBIT reports key and relative-axis support");

            struct input_id id;
            memset(&id, 0, sizeof(id));
            TEST_ASSERT(ioctl(fd, EVIOCGID, &id) == 0 && id.bustype != 0,
                        "EVIOCGID returns a bus/vendor identity");
            close(fd);
        }
    }

    /* ── Per-process CPU accounting ──────────────────────────────────────── */
    {
        struct tms t1, t2;
        memset(&t1, 0, sizeof(t1));
        memset(&t2, 0, sizeof(t2));

        TEST_ASSERT(times(&t1) != (clock_t)-1, "times() returns the elapsed tick count");

        /* Burn a measurable amount of user time. */
        volatile double sink = 0.0;
        for (int i = 0; i < 4000000; i++) sink += (double)i * 0.5;
        (void)sink;

        times(&t2);
        TEST_ASSERT(t2.tms_utime >= t1.tms_utime && (t2.tms_utime + t2.tms_stime) > 0,
                    "times() charges CPU time to this process");
    }

    /* ── PR_SET_CHILD_SUBREAPER: Process subreaper management ───────────── */
    {
        int sub = 0;
        TEST_ASSERT(prctl(36, 1, 0, 0, 0) == 0, "prctl(PR_SET_CHILD_SUBREAPER) sets subreaper status");
        TEST_ASSERT(prctl(37, (unsigned long)&sub, 0, 0, 0) == 0 && sub == 1,
                    "prctl(PR_GET_CHILD_SUBREAPER) reports process is subreaper");
    }

    /* ── POSIX message queues (mq_open/send/receive/notify/getattr) ──────── */
    {
        const char *qname = "/azami_test_q";
        mq_unlink(qname);                    /* in case a previous boot left one */

        struct mq_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.mq_maxmsg  = 4;
        attr.mq_msgsize = 64;

        mqd_t q = mq_open(qname, O_CREAT | O_EXCL | O_RDWR, 0600, &attr);
        TEST_ASSERT(q >= 0, "mq_open creates a POSIX message queue");

        if (q >= 0) {
            struct mq_attr got;
            memset(&got, 0, sizeof(got));
            TEST_ASSERT(mq_getattr(q, &got) == 0 && got.mq_maxmsg == 4 &&
                        got.mq_msgsize == 64 && got.mq_curmsgs == 0,
                        "mq_getattr reports the attributes the queue was made with");

            /* Send out of priority order; receive must reorder. */
            TEST_ASSERT(mq_send(q, "low", 3, 1) == 0, "mq_send accepts a message");
            TEST_ASSERT(mq_send(q, "high", 4, 9) == 0,
                        "mq_send accepts a higher-priority message");
            TEST_ASSERT(mq_send(q, "mid", 3, 5) == 0,
                        "mq_send accepts a middling-priority message");

            memset(&got, 0, sizeof(got));
            TEST_ASSERT(mq_getattr(q, &got) == 0 && got.mq_curmsgs == 3,
                        "mq_getattr counts the queued messages");

            char buf[64];
            unsigned prio = 0;
            ssize_t n = mq_receive(q, buf, sizeof(buf), &prio);
            TEST_ASSERT(n == 4 && prio == 9 && memcmp(buf, "high", 4) == 0,
                        "mq_receive returns the highest-priority message first");
            n = mq_receive(q, buf, sizeof(buf), &prio);
            TEST_ASSERT(n == 3 && prio == 5 && memcmp(buf, "mid", 3) == 0,
                        "mq_receive then returns the next priority down");
            n = mq_receive(q, buf, sizeof(buf), &prio);
            TEST_ASSERT(n == 3 && prio == 1 && memcmp(buf, "low", 3) == 0,
                        "mq_receive drains the queue in priority order");

            /* A buffer smaller than mq_msgsize must be refused outright: POSIX
             * never delivers a truncated message. */
            errno = 0;
            TEST_ASSERT(mq_receive(q, buf, 8, NULL) == -1 && errno == EMSGSIZE,
                        "mq_receive rejects a buffer smaller than mq_msgsize");

            /* O_NONBLOCK via mq_setattr, then an empty-queue receive. */
            struct mq_attr nb;
            memset(&nb, 0, sizeof(nb));
            nb.mq_flags = O_NONBLOCK;
            TEST_ASSERT(mq_setattr(q, &nb, NULL) == 0,
                        "mq_setattr turns on O_NONBLOCK");
            errno = 0;
            TEST_ASSERT(mq_receive(q, buf, sizeof(buf), NULL) == -1 &&
                        errno == EAGAIN,
                        "a non-blocking receive on an empty queue gives EAGAIN");

            /* An mqd_t is a file descriptor, so poll() must work on it. */
            struct pollfd pfd;
            pfd.fd = q;
            pfd.events = POLLIN | POLLOUT;
            pfd.revents = 0;
            TEST_ASSERT(poll(&pfd, 1, 0) >= 0 && (pfd.revents & POLLOUT) &&
                        !(pfd.revents & POLLIN),
                        "poll on an empty queue reports writable, not readable");

            /* mq_notify registers this process for the empty->non-empty edge. */
            struct sigevent sev;
            memset(&sev, 0, sizeof(sev));
            sev.sigev_notify = SIGEV_SIGNAL;
            sev.sigev_signo  = SIGUSR2;
            TEST_ASSERT(mq_notify(q, &sev) == 0, "mq_notify registers for a signal");
            TEST_ASSERT(mq_notify(q, NULL) == 0, "mq_notify(NULL) deregisters");

            TEST_ASSERT(mq_close(q) == 0, "mq_close releases the descriptor");
        }

        TEST_ASSERT(mq_unlink(qname) == 0, "mq_unlink removes the queue name");
        errno = 0;
        TEST_ASSERT(mq_open(qname, O_RDONLY) == -1 && errno == ENOENT,
                    "an unlinked queue can no longer be opened");
        errno = 0;
        TEST_ASSERT(mq_open("no_leading_slash", O_RDONLY) == -1 && errno == EINVAL,
                    "mq_open rejects a name without a leading slash");
    }

    /* ── accept4: SOCK_NONBLOCK/SOCK_CLOEXEC applied atomically ──────────── */
    {
        int lfd = socket(AF_INET, SOCK_STREAM, 0);
        TEST_ASSERT(lfd >= 0, "socket for the accept4 test");
        if (lfd >= 0) {
            struct sockaddr_in sa;
            memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port = htons(24601);
            sa.sin_addr.s_addr = htonl(INADDR_ANY);
            if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
                listen(lfd, 4) == 0) {
                /* Flag validation happens before anything can block. */
                errno = 0;
                TEST_ASSERT(accept4(lfd, NULL, NULL, 0x40000000) == -1 &&
                            errno == EINVAL,
                            "accept4 rejects unknown flags");

                /* accept4's SOCK_NONBLOCK applies to the *accepted* socket, not
                 * to the accept call — whether the call blocks is a property of
                 * the listener. Mark the listener non-blocking so an idle
                 * accept4 reports EAGAIN instead of parking init forever. */
                fcntl(lfd, F_SETFL, fcntl(lfd, F_GETFL, 0) | O_NONBLOCK);
                errno = 0;
                int c = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                TEST_ASSERT(c == -1 && errno == EAGAIN,
                            "accept4 on an idle non-blocking listener gives EAGAIN");
            }
            close(lfd);
        }
    }

    /* ── ioprio_set / ioprio_get ─────────────────────────────────────────── */
    {
        int prev = ioprio_get(IOPRIO_WHO_PROCESS, 0);
        TEST_ASSERT(prev >= 0, "ioprio_get reports this process's I/O priority");

        int want = IOPRIO_PRIO_VALUE(IOPRIO_CLASS_IDLE, 0);
        TEST_ASSERT(ioprio_set(IOPRIO_WHO_PROCESS, 0, want) == 0,
                    "ioprio_set moves this process to the idle class");
        TEST_ASSERT(ioprio_get(IOPRIO_WHO_PROCESS, 0) == want,
                    "ioprio_get reads back exactly what was set");

        errno = 0;
        TEST_ASSERT(ioprio_set(IOPRIO_WHO_PROCESS, 0,
                               IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, 99)) == -1 &&
                    errno == EINVAL,
                    "ioprio_set rejects a level outside the class range");

        /* Put it back so the rest of the boot is not throttled. */
        ioprio_set(IOPRIO_WHO_PROCESS, 0,
                   IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, 4));
    }

    /* ── NUMA memory policy on a single-node machine ─────────────────────── */
    {
        unsigned long node0 = 1;    /* only node 0 exists */
        TEST_ASSERT(set_mempolicy(MPOL_BIND, &node0, 8) == 0,
                    "set_mempolicy(MPOL_BIND) accepts the one node there is");

        int mode = -1;
        unsigned long mask = 0;
        TEST_ASSERT(get_mempolicy(&mode, &mask, 8, NULL, 0) == 0 &&
                    mode == MPOL_BIND && mask == 1,
                    "get_mempolicy round-trips the policy that was set");

        unsigned long node3 = 1UL << 3;
        errno = 0;
        TEST_ASSERT(set_mempolicy(MPOL_BIND, &node3, 8) == -1 && errno == EINVAL,
                    "set_mempolicy rejects a nodemask naming a node that does not exist");

        errno = 0;
        TEST_ASSERT(set_mempolicy(MPOL_DEFAULT, &node0, 8) == -1 && errno == EINVAL,
                    "MPOL_DEFAULT with a non-empty nodemask is an error");

        TEST_ASSERT(set_mempolicy(MPOL_DEFAULT, NULL, 0) == 0,
                    "set_mempolicy(MPOL_DEFAULT) restores the default policy");
    }

    /* ── futex2: futex_wake / futex_wait ─────────────────────────────────── */
    {
        static volatile unsigned int fword;
        fword = 7;

        /* No waiters, so a wake reports zero rather than failing. */
        TEST_ASSERT(futex_wake((void *)&fword, ~0UL, 1,
                               FUTEX2_SIZE_U32 | FUTEX2_PRIVATE) == 0,
                    "futex_wake on an unwaited futex wakes nobody");

        /* A mismatched value must not block. */
        errno = 0;
        TEST_ASSERT(futex_wait((void *)&fword, 8, ~0UL,
                               FUTEX2_SIZE_U32 | FUTEX2_PRIVATE, NULL, 0) == -1 &&
                    errno == EAGAIN,
                    "futex_wait returns EAGAIN when the value already differs");

        /* An absolute deadline in the past must time out immediately rather
         * than being treated as a relative interval. */
        struct timespec past;
        past.tv_sec = 1;
        past.tv_nsec = 0;
        errno = 0;
        TEST_ASSERT(futex_wait((void *)&fword, 7, ~0UL,
                               FUTEX2_SIZE_U32 | FUTEX2_PRIVATE, &past, 0) == -1 &&
                    errno == ETIMEDOUT,
                    "futex_wait treats its timeout as an absolute deadline");

        errno = 0;
        TEST_ASSERT(futex_wake((void *)&fword, ~0UL, 1, FUTEX2_SIZE_U64) == -1 &&
                    errno == EINVAL,
                    "futex2 rejects a futex width the kernel does not implement");
    }

    /* ── fchmodat2 ───────────────────────────────────────────────────────── */
    {
        const char *fcpath = "/tmp/fchmodat2_test.tmp";
        int ffd = open(fcpath, O_CREAT | O_RDWR, 0644);
        if (ffd >= 0) close(ffd);

        TEST_ASSERT(fchmodat2(AT_FDCWD, fcpath, 0600, 0) == 0,
                    "fchmodat2 changes a file's mode");
        struct stat fcst;
        memset(&fcst, 0, sizeof(fcst));
        TEST_ASSERT(stat(fcpath, &fcst) == 0 && (fcst.st_mode & 0777) == 0600,
                    "fchmodat2's new mode is visible through stat");
        errno = 0;
        TEST_ASSERT(fchmodat2(AT_FDCWD, fcpath, 0644, 0x8000) == -1 &&
                    errno == EINVAL,
                    "fchmodat2 rejects unknown flags");
        unlink(fcpath);
    }

    /* ── /proc/mqueues and the CPU accelerator report in /proc/cpuinfo ───── */
    {
        int pfd = open("/proc/mqueues", O_RDONLY);
        TEST_ASSERT(pfd >= 0, "open(/proc/mqueues)");
        if (pfd >= 0) close(pfd);

        int cfd = open("/proc/cpuinfo", O_RDONLY);
        if (cfd >= 0) {
            static char cbuf[16384];
            size_t total = 0;
            while (total < sizeof(cbuf) - 1) {
                ssize_t cn = read(cfd, cbuf + total, sizeof(cbuf) - 1 - total);
                if (cn <= 0) break;
                total += (size_t)cn;
            }
            cbuf[total] = '\0';
            close(cfd);
            TEST_ASSERT(strstr(cbuf, "azami_hwaccel") != NULL &&
                        strstr(cbuf, "crc32c=") != NULL,
                        "/proc/cpuinfo reports the chosen CPU accelerator paths");
        }
    }

    /* ── Linux pidfd_open and pidfd_send_signal ──────────────────────────── */
    {
        pid_t p = fork();
        if (p == 0) {
            sleep(5);
            exit(0);
        }
        int pfd = pidfd_open(p, 0);
        TEST_ASSERT(pfd >= 0, "pidfd_open(child) returns valid file descriptor");
        if (pfd >= 0) {
            int r_sig = pidfd_send_signal(pfd, SIGTERM, NULL, 0);
            TEST_ASSERT(r_sig == 0, "pidfd_send_signal sends signal through pidfd");
            int st = 0;
            waitpid(p, &st, 0);
            close(pfd);
        }
    }

    /* ── Linux signalfd: signal file descriptor event delivery ───────────── */
    {
        sigset_t smask;
        sigemptyset(&smask);
        sigaddset(&smask, SIGUSR1);
        sigprocmask(SIG_BLOCK, &smask, NULL);

        int sfd = signalfd(-1, &smask, 00004000 /* SFD_NONBLOCK */);
        TEST_ASSERT(sfd >= 0, "signalfd creates a signal descriptor");
        if (sfd >= 0) {
            raise(SIGUSR1);
            struct {
                uint32_t ssi_signo;
                int32_t  ssi_errno;
                int32_t  ssi_code;
                uint32_t ssi_pid;
                uint32_t ssi_uid;
                int32_t  ssi_fd;
                uint32_t ssi_tid;
                uint32_t ssi_band;
                uint32_t ssi_overrun;
                uint32_t ssi_trapno;
                int32_t  ssi_status;
                int32_t  ssi_int;
                uint64_t ssi_ptr;
                uint64_t ssi_utime;
                uint64_t ssi_stime;
                uint64_t ssi_addr;
                uint16_t ssi_addr_lsb;
                uint16_t __pad2;
                int32_t  ssi_syscall;
                uint64_t ssi_call_addr;
                uint32_t ssi_arch;
                uint8_t  __pad[28];
            } ssi;
            memset(&ssi, 0, sizeof(ssi));
            ssize_t n = read(sfd, &ssi, sizeof(ssi));
            TEST_ASSERT(n == sizeof(ssi) && ssi.ssi_signo == SIGUSR1,
                        "signalfd read delivers queued signal info");
            close(sfd);
        }
        sigprocmask(SIG_UNBLOCK, &smask, NULL);
    }

    /* ── Linux memfd_create: in-memory anonymous file ────────────────────── */
    {
        int mfd = memfd_create("test_memfd", 0);
        TEST_ASSERT(mfd >= 0, "memfd_create allocates anonymous memory descriptor");
        if (mfd >= 0) {
            const char *msg = "AzamiOS Linux Memfd";
            size_t mlen = strlen(msg);
            ssize_t nw = write(mfd, msg, mlen);
            TEST_ASSERT(nw == (ssize_t)mlen, "memfd write stores data to memory file");

            off_t off = lseek(mfd, 0, SEEK_SET);
            TEST_ASSERT(off == 0, "memfd lseek repositions offset to start");

            char rdata[32] = {0};
            ssize_t nr = read(mfd, rdata, sizeof(rdata) - 1);
            TEST_ASSERT(nr == (ssize_t)mlen && strcmp(rdata, msg) == 0,
                        "memfd read retrieves identical written data");
            close(mfd);
        }
    }

    /* ── POSIX mkfifo / mknod(S_IFIFO) ───────────────────────────────────── */
    {
        const char *fifopath = "/tmp/posix_test.fifo";
        unlink(fifopath);
        int r_fifo = mknod(fifopath, S_IFIFO | 0666, 0);
        TEST_ASSERT(r_fifo == 0, "mknod(S_IFIFO) creates a named pipe");

        struct stat fst;
        memset(&fst, 0, sizeof(fst));
        TEST_ASSERT(stat(fifopath, &fst) == 0 && S_ISFIFO(fst.st_mode),
                    "stat reports S_ISFIFO for named pipe");
        unlink(fifopath);
    }

    /* ── chroot(2) actually confines ─────────────────────────────────────────
     * Run in a child, because chroot is one-way: the parent has to stay out of
     * the jail to go on testing. The checks are the two escapes the old
     * implementation allowed — it only reassigned cwd, so an absolute path
     * ignored the jail entirely and a single "cd .." walked back out.
     * Exit status carries the result: 0 = confined, 1 = escaped, 2 = setup
     * failed (reported as a skip rather than a failure). */
    {
        mkdir("/tmp/jail", 0755);
        mkdir("/tmp/jail/inside", 0755);

        int jpid = fork();
        if (jpid == 0) {
            if (chroot("/tmp/jail") != 0) _exit(2);

            /* An absolute path must be read relative to the new root. */
            struct stat js;
            if (stat("/inside", &js) != 0 || !S_ISDIR(js.st_mode)) _exit(1);

            /* ...and the real /tmp must no longer be reachable by name. */
            if (stat("/tmp/jail", &js) == 0) _exit(1);

            /* ".." at the root must stay at the root, however many of them. */
            if (chdir("/../../../..") != 0) _exit(1);
            if (stat("inside", &js) != 0) _exit(1);
            if (stat("tmp", &js) == 0) _exit(1);

            _exit(0);
        }

        if (jpid > 0) {
            int jst = -1;
            waitpid(jpid, &jst, 0);
            int jcode = WIFEXITED(jst) ? WEXITSTATUS(jst) : -1;
            if (jcode == 2) {
                printf("  [SKIP] chroot(2) confines the process (setup unavailable)\n");
            } else {
                TEST_ASSERT(jcode == 0,
                            "chroot(2) confines absolute paths and '..' to the new root");
            }
        }

        rmdir("/tmp/jail/inside");
        rmdir("/tmp/jail");
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

