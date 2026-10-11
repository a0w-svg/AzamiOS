#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

static int passed, failed, skipped, azami, have_clone3;
static const char *program;

static void result(const char *name, long value, long expected)
{
    int ok = value == expected;
    printf("NSCASE {\"name\":\"%s\",\"value\":%ld,\"expected\":%ld,\"passed\":%s}\n",
           name, value, expected, ok ? "true" : "false");
    if (ok) passed++; else failed++;
}

#define CHECK(name, condition) result(name, !!(condition), 1)
#define EXPECT(name, expression, expected) do { \
    errno = 0; long test_value = (expression); \
    result(name, test_value == -1 ? -(long)errno : test_value, expected); \
} while (0)

static void skip(const char *name, const char *reason)
{
    printf("NSSKIP {\"name\":\"%s\",\"reason\":\"%s\"}\n", name, reason);
    skipped++;
}

static int wait_ok(pid_t child)
{
    int status;
    pid_t rc;
    if (child <= 0) return 0;
    do { rc = waitpid(child, &status, 0); } while (rc < 0 && errno == EINTR);
    if (rc != child || !WIFEXITED(status) || WEXITSTATUS(status)) {
        fprintf(stderr, "child %ld wait/status failure: %ld/%#x\n", (long)child,
                (long)rc, rc == child ? status : 0);
        return 0;
    }
    return 1;
}

static int token(int fd, int writing)
{
    unsigned char byte = 0x5a;
    ssize_t rc;
    do { rc = writing ? write(fd, &byte, 1) : read(fd, &byte, 1); }
    while (rc < 0 && errno == EINTR);
    return rc == 1 && byte == 0x5a;
}

static int ns_link(pid_t pid, const char *kind, char output[128])
{
    char path[80];
    if (pid) snprintf(path, sizeof(path), "/proc/%ld/ns/%s", (long)pid, kind);
    else snprintf(path, sizeof(path), "/proc/self/ns/%s", kind);
    ssize_t n = readlink(path, output, 127);
    if (n <= 0 || n >= 127) return -1;
    output[n] = '\0';
    return 0;
}

static int valid_uts_link(const char *text)
{
    uint64_t id;
    int end = 0;
    return sscanf(text, "uts:[%" SCNu64 "]%n", &id, &end) == 1 && id &&
           end > 0 && text[end] == '\0';
}

struct clone_args {
    uint64_t flags, pidfd, child_tid, parent_tid, exit_signal;
    uint64_t stack, stack_size, tls, set_tid, set_tid_size, cgroup;
};

static long ns_clone(int api, uint64_t flags)
{
    if (api == 3) {
        struct clone_args args = { .flags = flags, .exit_signal = SIGCHLD };
        return syscall(SYS_clone3, &args, sizeof(args));
    }
    return syscall(SYS_clone, flags | SIGCHLD, NULL, NULL, NULL, 0);
}

static long rejected_clone(int api, uint64_t flags)
{
    errno = 0;
    long rc = ns_clone(api, flags);
    if (!rc) _exit(99); /* Unexpected children cannot leak into the test supervisor. */
    if (rc > 0) { (void)wait_ok((pid_t)rc); return 1; }
    return -(long)errno;
}

static void clone_isolation(int api)
{
    char before[128], after[128], name[80];
    if (ns_link(0, "uts", before)) { CHECK("clone.setup", 0); return; }
    struct utsname original;
    if (uname(&original)) { CHECK("clone.uname_setup", 0); return; }
    long child = ns_clone(api, CLONE_NEWUTS);
    if (!child) {
        struct utsname info;
        if (ns_link(0, "uts", after) || !strcmp(before, after) ||
            sethostname("clone-child", 11) || setdomainname("clone-domain", 12) ||
            uname(&info) || strcmp(info.nodename, "clone-child") ||
            strcmp(info.domainname, "clone-domain")) _exit(1);
        _exit(0);
    }
    snprintf(name, sizeof(name), "clone%d.uts_isolation", api);
    CHECK(name, child > 0 && wait_ok((pid_t)child));
    snprintf(name, sizeof(name), "clone%d.parent_unchanged", api);
    struct utsname current;
    CHECK(name, !uname(&current) && !ns_link(0, "uts", after) &&
          !strcmp(before, after) && !strcmp(original.nodename, current.nodename) &&
          !strcmp(original.domainname, current.domainname));
}

static void test_names(void)
{
    struct utsname info;
    char full[65];
    memset(full, 'N', 64); full[64] = 0;
    EXPECT("hostname.length64", syscall(SYS_sethostname, full, 64), 0);
    CHECK("hostname.length64_readback", !uname(&info) && !memcmp(info.nodename, full, 65));
    EXPECT("hostname.length65", syscall(SYS_sethostname, full, 65), -EINVAL);
    EXPECT("hostname.invalid_pointer", syscall(SYS_sethostname, (void *)1, 4), -EFAULT);
    EXPECT("hostname.kernel_pointer", syscall(SYS_sethostname, (void *)(uintptr_t)-4096, 4), -EFAULT);
    EXPECT("hostname.null_nonempty", syscall(SYS_sethostname, NULL, 1), -EFAULT);
    EXPECT("hostname.empty", syscall(SYS_sethostname, NULL, 0), 0);
    CHECK("hostname.empty_readback", !uname(&info) && info.nodename[0] == 0);
    EXPECT("hostname.embedded_nul", syscall(SYS_sethostname, "a\0b", 3), 0);
    CHECK("hostname.embedded_nul_readback", !uname(&info) && !memcmp(info.nodename, "a\0b\0", 4));
    EXPECT("domain.length64", syscall(SYS_setdomainname, full, 64), 0);
    CHECK("domain.length64_readback", !uname(&info) && !memcmp(info.domainname, full, 65));
    EXPECT("domain.length65", syscall(SYS_setdomainname, full, 65), -EINVAL);
    EXPECT("domain.invalid_pointer", syscall(SYS_setdomainname, (void *)1, 4), -EFAULT);
    EXPECT("domain.empty", syscall(SYS_setdomainname, NULL, 0), 0);
    CHECK("domain.empty_readback", !uname(&info) && info.domainname[0] == 0);
    EXPECT("hostname.restore_test_name", sethostname("semantic-parent", 15), 0);
    EXPECT("domain.restore_test_name", setdomainname("semantic-domain", 15), 0);
}

static void test_fork_and_exec(void)
{
    char parent_link[128], after[128];
    if (ns_link(0, "uts", parent_link)) { CHECK("fork.setup", 0); return; }
    pid_t child = fork();
    if (!child) {
        if (ns_link(0, "uts", after) || strcmp(parent_link, after) ||
            sethostname("fork-shared", 11)) _exit(1);
        _exit(0);
    }
    CHECK("fork.shares_uts_identity", child > 0 && wait_ok(child));
    struct utsname info;
    CHECK("fork.shared_name_visible", !uname(&info) && !strcmp(info.nodename, "fork-shared"));
    EXPECT("hostname.restore_after_fork", sethostname("semantic-parent", 15), 0);

    child = fork();
    if (!child) {
        if (unshare(CLONE_NEWUTS) || ns_link(0, "uts", after) || !strcmp(parent_link, after) ||
            sethostname("exec-private", 12) || setdomainname("exec-domain", 11)) _exit(1);
        char *args[] = {(char *)program, "--verify-exec", "exec-private", "exec-domain", after, NULL};
        char *env[] = {NULL};
        execve(program, args, env);
        perror("exec namespace fixture");
        _exit(2);
    }
    CHECK("fork.private_names_survive_exec", child > 0 && wait_ok(child));
    CHECK("fork.private_change_hidden", !uname(&info) && !strcmp(info.nodename, "semantic-parent") &&
          !strcmp(info.domainname, "semantic-domain") && !ns_link(0, "uts", after) && !strcmp(parent_link, after));
}

static void test_rejections(void)
{
    if (!azami) { skip("unsupported_namespaces", "Linux implements the namespace types rejected by AzamiOS"); return; }
    uint64_t flags[] = {CLONE_NEWNS, CLONE_NEWCGROUP, CLONE_NEWIPC, CLONE_NEWUSER,
                        CLONE_NEWPID, CLONE_NEWNET, 0x80 /* CLONE_NEWTIME */,
                        CLONE_FS, CLONE_FILES};
    char before[128], after[128], name[80];
    struct utsname original, current;
    if (ns_link(0, "uts", before) || uname(&original)) { CHECK("reject.setup", 0); return; }
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
        snprintf(name, sizeof(name), "unshare.unsupported_%08" PRIx64, flags[i]);
        EXPECT(name, syscall(SYS_unshare, flags[i]), -EOPNOTSUPP);
        snprintf(name, sizeof(name), "unshare.mixed_%08" PRIx64, flags[i]);
        EXPECT(name, syscall(SYS_unshare, flags[i] | CLONE_NEWUTS), -EOPNOTSUPP);
        snprintf(name, sizeof(name), "unshare.atomic_%08" PRIx64, flags[i]);
        CHECK(name, !ns_link(0, "uts", after) && !strcmp(before, after) && !uname(&current) &&
              !memcmp(original.nodename, current.nodename, 65) &&
              !memcmp(original.domainname, current.domainname, 65));
        if (i < 7) {
            /* Legacy clone's low byte is the exit signal, so NEWTIME is only
             * tested via clone3. FS/FILES sharing is a separate ABI milestone. */
            if (i != 6) {
                snprintf(name, sizeof(name), "clone.unsupported_%08" PRIx64, flags[i]);
                result(name, rejected_clone(1, flags[i]), -EOPNOTSUPP);
            }
            snprintf(name, sizeof(name), "clone3.unsupported_%08" PRIx64, flags[i]);
            result(name, rejected_clone(3, flags[i]), -EOPNOTSUPP);
        }
    }
    result("clone.thread_local_uts_unsupported",
           rejected_clone(1, CLONE_VM | CLONE_SIGHAND | CLONE_THREAD | CLONE_NEWUTS), -EOPNOTSUPP);
}

static void *sibling(void *argument)
{
    int *state = argument;
    __atomic_store_n(state, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(state, __ATOMIC_ACQUIRE) != 2) sched_yield();
    return NULL;
}

static void test_thread_scope(void)
{
    if (!azami) { skip("thread_local_namespace", "AzamiOS explicitly rejects per-thread unshare"); return; }
    char before[128], after[128];
    if (ns_link(0, "uts", before)) { CHECK("thread.setup", 0); return; }
    int state = 0;
    pthread_t thread;
    int rc = pthread_create(&thread, NULL, sibling, &state);
    CHECK("thread.create", rc == 0);
    if (rc) return;
    while (!__atomic_load_n(&state, __ATOMIC_ACQUIRE)) sched_yield();
    EXPECT("thread.unshare_unsupported", unshare(CLONE_NEWUTS), -EOPNOTSUPP);
    CHECK("thread.namespace_unchanged", !ns_link(0, "uts", after) && !strcmp(before, after));
    __atomic_store_n(&state, 2, __ATOMIC_RELEASE);
    CHECK("thread.join", pthread_join(thread, NULL) == 0);
}

static void test_namespace_lifetime(void)
{
    int ready[2], release[2];
    if (pipe(ready) || pipe(release)) { CHECK("lifetime.pipe", 0); return; }
    pid_t child = fork();
    if (!child) {
        close(ready[0]); close(release[1]);
        for (int i = 0; i < 500; i++) {
            if (unshare(CLONE_NEWUTS)) _exit(1);
            if (!i && !token(ready[1], 1)) _exit(2);
        }
        int ok = token(release[0], 0);
        close(ready[1]); close(release[0]);
        _exit(ok ? 0 : 3);
    }
    close(ready[1]); close(release[0]);
    int ok = child > 0 && token(ready[0], 0);
    char text[128], parent[128];
    if (ns_link(0, "uts", parent)) ok = 0;
    uint64_t previous = 0;
    for (int i = 0; i < 3000 && ok; i++) {
        if (ns_link(child, "uts", text) || !valid_uts_link(text) || !strcmp(parent, text)) ok = 0;
        if (azami && ok) {
            uint64_t id = strtoull(text + 5, NULL, 10);
            if (id < previous) ok = 0;
            previous = id;
        }
    }
    CHECK("lifetime.concurrent_procfs_snapshot", ok);
    if (child > 0 && !token(release[1], 1)) ok = 0;
    close(ready[0]); close(release[1]);
    CHECK("lifetime.child_reaped", child > 0 && wait_ok(child) && ok);
    if (child > 0) {
        errno = 0;
        CHECK("lifetime.dead_process_link_absent", ns_link(child, "uts", text) < 0 && errno == ENOENT);
    }
}

static void test_name_atomicity(void)
{
    char seed[65]; memset(seed, 'A', 64); seed[64] = 0;
    if (sethostname(seed, 64)) { CHECK("atomicity.setup", 0); return; }
    pid_t children[2] = {0};
    int ok = 1;
    for (int i = 0; i < 2; i++) {
        children[i] = fork();
        if (!children[i]) {
            char text[64]; memset(text, 'A' + i, 64);
            for (int j = 0; j < 3000; j++) if (sethostname(text, 64)) _exit(1);
            _exit(0);
        }
        if (children[i] < 0) ok = 0;
    }
    for (int i = 0; i < 3000 && ok; i++) {
        struct utsname info;
        if (uname(&info) || (info.nodename[0] != 'A' && info.nodename[0] != 'B') || info.nodename[64]) ok = 0;
        for (int j = 1; j < 64 && ok; j++) if (info.nodename[j] != info.nodename[0]) ok = 0;
    }
    CHECK("atomicity.uname_no_torn_name", ok);
    for (int i = 0; i < 2; i++) {
        char name[64]; snprintf(name, sizeof(name), "atomicity.writer%d_reaped", i);
        CHECK(name, children[i] > 0 && wait_ok(children[i]));
    }
    EXPECT("hostname.restore_after_atomicity", sethostname("semantic-parent", 15), 0);
}

static void test_unprivileged(void)
{
    pid_t child = fork();
    if (!child) {
        struct { uint32_t version; int pid; } header = {0x20080522, 0};
        struct { uint32_t effective, permitted, inheritable; } data[2] = {{0}, {0}};
        if (syscall(SYS_capset, &header, data)) _exit(1);
        errno = 0;
        if (unshare(CLONE_NEWUTS) != -1 || errno != EPERM) _exit(2);
        if (rejected_clone(1, CLONE_NEWUTS) != -EPERM) _exit(3);
        if (have_clone3 && rejected_clone(3, CLONE_NEWUTS) != -EPERM) _exit(4);
        errno = 0;
        if (syscall(SYS_sethostname, NULL, 0) != -1 || errno != EPERM) _exit(5);
        errno = 0;
        if (syscall(SYS_setdomainname, NULL, 0) != -1 || errno != EPERM) _exit(6);
        int fd = open("/proc/self/status", O_RDONLY);
        if (fd < 0) _exit(7);
        errno = 0;
        if (syscall(SYS_setns, fd, 0) != -1 || errno != EINVAL) _exit(8);
        close(fd);
        errno = 0;
        if (syscall(SYS_setns, -1, 0) != -1 || errno != EBADF) _exit(9);
        _exit(0);
    }
    CHECK("permissions.capability_drop_blocks_creation_and_names", child > 0 && wait_ok(child));
}

int main(int argc, char **argv)
{
    program = argv[0];
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 5 && !strcmp(argv[1], "--verify-exec")) {
        struct utsname info;
        char text[128];
        return uname(&info) || ns_link(0, "uts", text) || strcmp(info.nodename, argv[2]) ||
               strcmp(info.domainname, argv[3]) || strcmp(text, argv[4]) ? 1 : 0;
    }
    if (argc != 1) return 2;
    struct utsname info;
    if (uname(&info)) return 1;
    azami = !strcmp(info.sysname, "AzamiOS");
    puts("== W3.8 namespace semantic suite ==");
    EXPECT("unshare.zero", unshare(0), 0);
    EXPECT("unshare.unknown_flag", syscall(SYS_unshare, UINT64_C(1) << 63), -EINVAL);
    EXPECT("setns.invalid_fd", syscall(SYS_setns, -1, 0), -EBADF);
    int fd = open("/proc/self/status", O_RDONLY);
    CHECK("setns.regular_fd_setup", fd >= 0);
    if (fd >= 0) { EXPECT("setns.regular_fd", syscall(SYS_setns, fd, 0), -EINVAL); close(fd); }
    char parent[128], current[128], child_pid_ns[128];
    CHECK("procfs.uts_link_format", !ns_link(0, "uts", parent) && valid_uts_link(parent));
    CHECK("procfs.pid_for_children_identity", !ns_link(0, "pid", current) &&
          !ns_link(0, "pid_for_children", child_pid_ns) && !strcmp(current, child_pid_ns));

    long probe = ns_clone(3, 0);
    if (!probe) _exit(0);
    if (probe < 0 && errno == ENOSYS && !azami) skip("clone3", "Linux host policy/kernel returned ENOSYS");
    else { CHECK("clone3.available", probe > 0 && wait_ok((pid_t)probe)); have_clone3 = probe > 0; }
    errno = 0;
    int rc = unshare(CLONE_NEWUTS);
    if (rc < 0 && !azami && errno == EPERM) {
        skip("privileged_uts", "Linux CAP_SYS_ADMIN unavailable; use disposable unshare -Ur");
        test_unprivileged();
    } else {
        result("unshare.uts", rc == -1 ? -(long)errno : rc, 0);
        if (!rc) {
            CHECK("unshare.new_identity", !ns_link(0, "uts", current) && strcmp(parent, current) != 0);
            test_names();
            test_fork_and_exec();
            clone_isolation(1);
            if (have_clone3) clone_isolation(3);
            test_rejections();
            test_thread_scope();
            test_namespace_lifetime();
            test_name_atomicity();
            test_unprivileged();
        }
    }
    printf("== namespaces skipped: %d ==\n", skipped);
    printf("== probe complete: %d passed, %d failed ==\n", passed, failed);
    return failed ? 1 : 0;
}
