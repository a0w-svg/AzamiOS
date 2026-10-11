#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

#define BENEATH 8
#define IN_ROOT 16
#define NO_SYMLINKS 4
#define NO_MAGICLINKS 2
struct how { uint64_t flags, mode, resolve; };
static int passed, failed, azami, base, outside;
static char fixture[256], root[256], external[256];
static int writer_failed, stop_writer, writer_ready;
static unsigned int writer_steps;

static int cleanup_path(const char *path, const struct stat *st, int type, struct FTW *walk)
{
    (void)st; (void)type; (void)walk;
    return remove(path);
}

static void record(const char *name, long value, long expected)
{
    int good = value == expected;
    printf("NSCASE {\"name\":\"%s\",\"value\":%ld,\"expected\":%ld,\"passed\":%s}\n",
           name, value, expected, good ? "true" : "false");
    if (good) passed++; else failed++;
}
static long open2(int dir, const char *path, uint64_t flags, uint64_t mode, uint64_t resolve)
{
    struct how h = {flags, mode, resolve};
    long fd = syscall(SYS_openat2, dir, path, &h, sizeof(h));
    return fd < 0 ? -errno : fd;
}
static long content(long fd)
{
    if (fd < 0) return fd;
    char value = 0;
    ssize_t n = read((int)fd, &value, 1);
    close((int)fd);
    return n == 1 ? value : -EIO;
}
static void probe(const char *name, const char *path, uint64_t flags, uint64_t resolve, long expected)
{
    record(name, content(open2(base, path, flags, 0, resolve)), expected);
}
static int write_marker(const char *path, char marker)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) return 0;
    int ok = write(fd, &marker, 1) == 1;
    return close(fd) == 0 && ok;
}
static int under(const char *parent, const char *name, char marker)
{
    char path[512]; snprintf(path, sizeof(path), "%s/%s", parent, name);
    return write_marker(path, marker);
}
static int make_link(const char *name, const char *target)
{
    char path[512]; snprintf(path, sizeof(path), "%s/%s", root, name);
    return symlink(target, path) == 0;
}
static int wait_ok(pthread_t thread)
{
    return pthread_join(thread, NULL) == 0 && !__atomic_load_n(&writer_failed, __ATOMIC_ACQUIRE);
}
static void *symlink_writer(void *unused)
{
    (void)unused;
    char temp[512], path[512];
    snprintf(temp, sizeof(temp), "%s/replacement", root);
    snprintf(path, sizeof(path), "%s/swap", root);
    __atomic_store_n(&writer_ready, 1, __ATOMIC_RELEASE);
    for (unsigned int i = 0; !__atomic_load_n(&stop_writer, __ATOMIC_ACQUIRE) || i < 100; i++) {
        if (symlink(i & 1 ? "inside" : "../outside/secret", temp) || rename(temp, path)) {
            __atomic_store_n(&writer_failed, 1, __ATOMIC_RELEASE); break;
        }
        __atomic_add_fetch(&writer_steps, 1, __ATOMIC_RELEASE);
    }
    return NULL;
}
static void *rename_writer(void *unused)
{
    (void)unused;
    char from[512], to[512];
    snprintf(from, sizeof(from), "%s/moving", root);
    snprintf(to, sizeof(to), "%s/moving", external);
    __atomic_store_n(&writer_ready, 1, __ATOMIC_RELEASE);
    for (unsigned int i = 0; !__atomic_load_n(&stop_writer, __ATOMIC_ACQUIRE) || i < 100; i++) {
        if (rename(from, to) || rename(to, from)) {
            __atomic_store_n(&writer_failed, 1, __ATOMIC_RELEASE); break;
        }
        __atomic_add_fetch(&writer_steps, 1, __ATOMIC_RELEASE);
    }
    return NULL;
}
static int slot;
static void *cwd_writer(void *unused)
{
    (void)unused;
    __atomic_store_n(&writer_ready, 1, __ATOMIC_RELEASE);
    for (unsigned int i = 0; !__atomic_load_n(&stop_writer, __ATOMIC_ACQUIRE) || i < 100; i++) {
        if (chdir(i & 1 ? root : external)) {
            __atomic_store_n(&writer_failed, 1, __ATOMIC_RELEASE); break;
        }
        __atomic_add_fetch(&writer_steps, 1, __ATOMIC_RELEASE);
    }
    return NULL;
}
static void *fd_writer(void *unused)
{
    (void)unused;
    __atomic_store_n(&writer_ready, 1, __ATOMIC_RELEASE);
    for (unsigned int i = 0; !__atomic_load_n(&stop_writer, __ATOMIC_ACQUIRE) || i < 100; i++) {
        close(slot);
        if (dup2(i & 1 ? base : outside, slot) != slot) {
            __atomic_store_n(&writer_failed, 1, __ATOMIC_RELEASE); break;
        }
        __atomic_add_fetch(&writer_steps, 1, __ATOMIC_RELEASE);
    }
    return NULL;
}
static void race(const char *name, void *(*writer)(void *), const char *path, uint64_t resolve, int reuse)
{
    writer_failed = stop_writer = writer_ready = 0; writer_steps = 0;
    pthread_t thread;
    if (pthread_create(&thread, NULL, writer, NULL)) { record(name, 0, 1); return; }
    while (!__atomic_load_n(&writer_ready, __ATOMIC_ACQUIRE)) sched_yield();
    int good = 1;
    for (int i = 0; i < 5000; i++) {
        long value = content(open2(reuse == 2 ? AT_FDCWD : (reuse ? slot : base), path, O_RDONLY, 0, resolve));
        int allowed = value == 'I' || value == -ENOENT || value == -EXDEV || value == -EAGAIN;
        if (resolve & NO_SYMLINKS) allowed = value == -ELOOP || value == -ENOENT;
        if (reuse) allowed = value == 'I' || value == 'X' || (reuse == 1 && value == -EBADF);
        if (!allowed) {
            printf("unexpected race result %s: %ld\n", name, value);
            good = 0; break;
        }
    }
    __atomic_store_n(&stop_writer, 1, __ATOMIC_RELEASE);
    good &= wait_ok(thread);
    good &= __atomic_load_n(&writer_steps, __ATOMIC_ACQUIRE) >= 100;
    record(name, good, 1);
}
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0); alarm(100);
    struct utsname u;
    if (uname(&u)) return 1;
    azami = !strcmp(u.sysname, "AzamiOS");
    strcpy(fixture, "/tmp/azami-openat2-XXXXXX");
    if (!mkdtemp(fixture)) return 1;
    snprintf(root, sizeof(root), "%.200s/root", fixture);
    snprintf(external, sizeof(external), "%.200s/outside", fixture);
    char sub[512], moving[512], path[512];
    snprintf(sub, sizeof(sub), "%s/sub", root);
    snprintf(moving, sizeof(moving), "%s/moving", root);
    int ok = !mkdir(root, 0700) && !mkdir(external, 0700) && !mkdir(sub, 0700) && !mkdir(moving, 0700) &&
             under(root, "inside", 'I') && under(external, "secret", 'X') &&
             under(external, "inside", 'X') && under(sub, "inside", 'I') &&
             make_link("relative", "inside") && make_link("escape", "../outside/secret") &&
             make_link("dirlink", "sub") && make_link("jump", "../outside") &&
             make_link("absinside", "/inside") && make_link("loop", "loop") &&
             make_link("swap", "inside");
    snprintf(path, sizeof(path), "%s/secret", external);
    ok &= make_link("absolute", path);
    record("fixture.ready", ok, 1);
    if (!ok) return 1;
    base = open(root, O_RDONLY | O_DIRECTORY); outside = open(external, O_RDONLY | O_DIRECTORY);
    record("fixture.dirfds", base >= 0 && outside >= 0, 1);
    if (base < 0 || outside < 0) return 1;
    probe("beneath.regular", "inside", 0, BENEATH, 'I');
    probe("beneath.relative_symlink", "relative", 0, BENEATH, 'I');
    probe("beneath.directory_symlink", "dirlink/inside", 0, BENEATH, 'I');
    probe("beneath.dotdot_inside", "sub/../inside", 0, BENEATH, 'I');
    probe("beneath.root_escape", "../outside/secret", 0, BENEATH, -EXDEV);
    probe("beneath.relative_escape", "escape", 0, BENEATH, -EXDEV);
    probe("beneath.absolute_escape", "absolute", 0, BENEATH, -EXDEV);
    probe("beneath.cancelled_escape", "jump/../root/inside", 0, BENEATH, -EXDEV);
    probe("beneath.absolute_input", path, 0, BENEATH, -EXDEV);
    probe("walk.missing_before_dotdot", "missing/../inside", 0, BENEATH, -ENOENT);
    probe("walk.nondir_before_dotdot", "inside/../inside", 0, BENEATH, -ENOTDIR);
    probe("walk.trailing_slash", "inside/", 0, BENEATH, -ENOTDIR);
    probe("walk.symlink_loop", "loop", 0, BENEATH, -ELOOP);
    probe("nosymlinks.final", "relative", 0, NO_SYMLINKS, -ELOOP);
    probe("nosymlinks.intermediate", "dirlink/inside", 0, NO_SYMLINKS, -ELOOP);
    probe("nosymlinks.cancelled", "dirlink/../inside", 0, NO_SYMLINKS, -ELOOP);
    probe("nofollow.final", "relative", O_NOFOLLOW, 0, -ELOOP);
    probe("nofollow.intermediate", "dirlink/inside", O_NOFOLLOW, 0, 'I');
    probe("exclusive.symlink", "relative", O_CREAT | O_EXCL, 0, -EEXIST);
    probe("exclusive.regular", "inside", O_CREAT | O_EXCL, BENEATH, -EEXIST);
    probe("inroot.absolute", "/inside", 0, IN_ROOT, 'I');
    probe("inroot.absolute_symlink", "absinside", 0, IN_ROOT, 'I');
    probe("inroot.dotdot_clamped", "../../inside", 0, IN_ROOT, 'I');
    probe("inroot.escape_rebased", "escape", 0, IN_ROOT, -ENOENT);
    probe("nomagic.ordinary_symlink", "relative", 0, NO_MAGICLINKS, 'I');
    snprintf(path, sizeof(path), "/proc/self/fd/%d", base);
    record("nomagic.proc_fd", content(open2(AT_FDCWD, path, 0, 0, NO_MAGICLINKS)), -ELOOP);
    snprintf(path, sizeof(path), "%s/inside", root);
    record("absolute.invalid_dirfd_ignored", content(open2(-1, path, 0, 0, 0)), 'I');
    record("relative.invalid_dirfd", open2(-1, "inside", 0, 0, 0), -EBADF);
    int regular = open(path, O_RDONLY);
    record("relative.nondir_fd", open2(regular, "inside", 0, 0, 0), -ENOTDIR); close(regular);
    record("path.empty", open2(base, "", 0, 0, 0), -ENOENT);
    record("path.null", open2(base, NULL, 0, 0, 0), -EFAULT);
    record("path.invalid_pointer", open2(base, (void *)1, 0, 0, 0), -EFAULT);
    record("how.mode_without_create", open2(base, "inside", 0, 0600, 0), -EINVAL);
    record("how.invalid_mode", open2(base, "new", O_CREAT, 010000, 0), -EINVAL);
    record("how.unknown_flags", open2(base, "inside", UINT64_C(1) << 63, 0, 0), -EINVAL);
    record("how.unknown_resolve", open2(base, "inside", 0, 0, UINT64_C(1) << 63), -EINVAL);
    record("how.conflicting_resolve", open2(base, "inside", 0, 0, BENEATH | IN_ROOT), -EINVAL);
    record("how.create_directory_conflict", open2(base, "new", O_CREAT | O_DIRECTORY, 0600, 0), -EINVAL);
    struct { struct how h; uint64_t extra; } ext = {{0}, 0};
    long fd = syscall(SYS_openat2, base, "inside", &ext, sizeof(ext));
    record("how.zero_extension", content(fd < 0 ? -errno : fd), 'I');
    ext.extra = 1; errno = 0;
    fd = syscall(SYS_openat2, base, "inside", &ext, sizeof(ext));
    record("how.nonzero_extension", fd < 0 ? -errno : fd, -E2BIG);
    fd = syscall(SYS_openat2, base, "inside", &ext, 8);
    record("how.short", fd < 0 ? -errno : fd, -EINVAL);
    fd = syscall(SYS_openat2, base, "inside", (void *)1, 24);
    record("how.invalid_pointer", fd < 0 ? -errno : fd, -EFAULT);
    fd = open2(base, "created", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600, BENEATH);
    struct stat st;
    record("create.mode_cloexec", fd >= 0 && !fstat((int)fd, &st) && (st.st_mode & 0777) == 0600 &&
           (fcntl((int)fd, F_GETFD) & FD_CLOEXEC), 1);
    if (fd >= 0) close((int)fd);
    record("create.escape", open2(base, "jump/new", O_CREAT | O_RDWR, 0600, BENEATH), -EXDEV);
    snprintf(path, sizeof(path), "%s/new", external);
    record("create.no_external_effect", access(path, F_OK) < 0 && errno == ENOENT, 1);
    fd = open2(base, "inside", 0, 0, BENEATH);
    snprintf(path, sizeof(path), "%s/inside", root);
    ok = fd >= 0 && !unlink(path) && !fstat((int)fd, &st) && st.st_nlink == 0;
    record("unlink.open_inode_survives", ok && content(fd) == 'I', 1);
    record("unlink.recreate", under(root, "inside", 'I'), 1);
    fd = open2(base, "inside", O_RDONLY | O_TRUNC, 0, BENEATH);
    record("truncate.readonly", fd >= 0 && !fstat((int)fd, &st) && st.st_size == 0, 1);
    if (fd >= 0) close((int)fd);
    record("truncate.restore", under(root, "inside", 'I'), 1);
    snprintf(sub, sizeof(sub), "%s/moving/child", root);
    mkdir(sub, 0700);
    snprintf(path, sizeof(path), "%s/moving/child/nested", root);
    errno = 0;
    record("rename.descendant_rejected", rename(moving, path) < 0 ? -errno : 0, -EINVAL);
    snprintf(sub, sizeof(sub), "%s/sub", root);
    int subfd = open(sub, O_RDONLY | O_DIRECTORY);
    snprintf(path, sizeof(path), "%s/moved-sub", external);
    ok = subfd >= 0 && !rename(sub, path);
    record("rename.dirfd_identity", ok && content(open2(subfd, "inside", 0, 0, BENEATH)) == 'I', 1);
    close(subfd);
    snprintf(sub, sizeof(sub), "%s/deleted", root); mkdir(sub, 0700);
    subfd = open(sub, O_RDONLY | O_DIRECTORY);
    ok = subfd >= 0 && !rmdir(sub) && !fstat(subfd, &st) && st.st_nlink == 0;
    record("rmdir.open_inode_survives", ok, 1);
    record("rmdir.no_children", open2(subfd, "missing", 0, 0, BENEATH), -ENOENT); close(subfd);
    race("race.symlink_beneath", symlink_writer, "swap", BENEATH, 0);
    race("race.symlink_no_follow", symlink_writer, "swap", NO_SYMLINKS, 0);
    race("race.rename_dotdot", rename_writer, "moving/../inside", BENEATH, 0);
    /* Keep the raced slot above ordinary lowest-free open allocations. */
    slot = fcntl(base, F_DUPFD, 32);
    if (slot >= 0) { race("race.fd_close_reuse", fd_writer, "inside", BENEATH, 1); close(slot); }
    else record("race.fd_close_reuse", 0, 1);
    char previous_cwd[512];
    if (getcwd(previous_cwd, sizeof(previous_cwd)) && !chdir(root)) {
        race("race.cwd_snapshot", cwd_writer, "inside", BENEATH, 2);
        record("cwd.restore", chdir(previous_cwd) == 0, 1);
    } else record("race.cwd_snapshot", 0, 1);
    if (azami) {
        record("unsupported.no_xdev", open2(base, "inside", 0, 0, 1), -EOPNOTSUPP);
        record("unsupported.cached", open2(base, "inside", 0, 0, 32), -EOPNOTSUPP);
        record("unsupported.path_fd", open2(base, "inside", O_PATH, 0, 0), -EOPNOTSUPP);
    } else puts("NSSKIP {\"name\":\"unsupported_modes\",\"reason\":\"AzamiOS explicitly rejects unimplemented modes\"}");
    close(base); close(outside);
    record("fixture.cleanup", nftw(fixture, cleanup_path, 16, FTW_DEPTH | FTW_PHYS) == 0, 1);
    printf("== namespaces skipped: %d ==\n", azami ? 0 : 1);
    printf("== probe complete: %d passed, %d failed ==\n", passed, failed);
    return failed != 0;
}
