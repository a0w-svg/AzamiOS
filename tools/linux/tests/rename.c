#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#define RENAME_EXCHANGE  (1 << 1)
#define RENAME_WHITEOUT  (1 << 2)
#endif

static int passed, failed;

static void record(const char *name, long value, long expected)
{
    int good = value == expected;
    printf("NSCASE {\"name\":\"%s\",\"value\":%ld,\"expected\":%ld,\"passed\":%s}\n",
           name, value, expected, good ? "true" : "false");
    if (good) passed++; else failed++;
}

static void fatal(const char *message)
{
    perror(message);
    exit(2);
}

static long do_renameat2(int olddfd, const char *oldpath, int newdfd, const char *newpath, unsigned int flags)
{
    long ret = syscall(SYS_renameat2, olddfd, oldpath, newdfd, newpath, flags);
    return ret < 0 ? -errno : ret;
}

static long do_renameat(int olddfd, const char *oldpath, int newdfd, const char *newpath)
{
    long ret = syscall(SYS_renameat, olddfd, oldpath, newdfd, newpath);
    return ret < 0 ? -errno : ret;
}

static long do_rename(const char *oldpath, const char *newpath)
{
    long ret = syscall(SYS_rename, oldpath, newpath);
    return ret < 0 ? -errno : ret;
}

static void create_file(const char *path, const char *content)
{
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) fatal("create_file open");
    size_t len = strlen(content);
    if (write(fd, content, len) != (ssize_t)len) fatal("create_file write");
    close(fd);
}

static int read_file(const char *path, char *buf, size_t max)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, max - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return 0;
}

static int is_dir(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

static int is_link(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    return S_ISLNK(st.st_mode);
}

struct race_state {
    int running;
    int bad;
    unsigned iters;
};

static void *race_worker(void *arg)
{
    struct race_state *s = arg;
    while (__atomic_load_n(&s->running, __ATOMIC_ACQUIRE)) {
        if (do_renameat2(AT_FDCWD, "race_a", AT_FDCWD, "race_b", RENAME_EXCHANGE) != 0) {
            s->bad = 1;
        }
        __atomic_add_fetch(&s->iters, 1, __ATOMIC_RELEASE);
        sched_yield();
    }
    return NULL;
}

static void run_race_test(void)
{
    create_file("race_a", "content_a");
    create_file("race_b", "content_b");

    struct race_state s = {.running = 1, .bad = 0, .iters = 0};
    pthread_t th;
    if (pthread_create(&th, NULL, race_worker, &s) != 0) fatal("pthread_create");

    char buf[32];
    int ok = 1;
    for (int i = 0; i < 2000; i++) {
        if (read_file("race_a", buf, sizeof(buf)) == 0) {
            if (strcmp(buf, "content_a") != 0 && strcmp(buf, "content_b") != 0) {
                ok = 0;
            }
        }
        if (read_file("race_b", buf, sizeof(buf)) == 0) {
            if (strcmp(buf, "content_a") != 0 && strcmp(buf, "content_b") != 0) {
                ok = 0;
            }
        }
        sched_yield();
    }

    __atomic_store_n(&s.running, 0, __ATOMIC_RELEASE);
    pthread_join(th, NULL);

    record("race.concurrent_rename", ok && !s.bad && s.iters >= 10, 1);

    unlink("race_a");
    unlink("race_b");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(60);

    /* Setup test directory */
    char testdir[128];
    snprintf(testdir, sizeof(testdir), "/tmp/test_rename_%d", (int)getpid());
    if (mkdir(testdir, 0755) != 0) fatal("mkdir testdir");
    if (chdir(testdir) != 0) fatal("chdir testdir");

    /* 1. Flag validation */
    create_file("f1", "1");
    create_file("f2", "2");
    record("flags.invalid_bit", do_renameat2(AT_FDCWD, "f1", AT_FDCWD, "f2", 0x80000000U), -EINVAL);
    record("flags.noreplace_and_exchange", do_renameat2(AT_FDCWD, "f1", AT_FDCWD, "f2", RENAME_NOREPLACE | RENAME_EXCHANGE), -EINVAL);
    record("flags.invalid_low_bit", do_renameat2(AT_FDCWD, "f1", AT_FDCWD, "f2", (1U << 3)), -EINVAL);

    /* 2. RENAME_NOREPLACE */
    record("noreplace.target_exists", do_renameat2(AT_FDCWD, "f1", AT_FDCWD, "f2", RENAME_NOREPLACE), -EEXIST);
    record("noreplace.same_path", do_renameat2(AT_FDCWD, "f1", AT_FDCWD, "f1", RENAME_NOREPLACE), -EEXIST);
    if (link("f1", "f1_link") != 0) fatal("link");
    record("noreplace.hardlink", do_renameat2(AT_FDCWD, "f1", AT_FDCWD, "f1_link", RENAME_NOREPLACE), -EEXIST);
    unlink("f1_link");
    record("noreplace.target_missing", do_renameat2(AT_FDCWD, "f1", AT_FDCWD, "f_new", RENAME_NOREPLACE), 0);
    record("noreplace.verified", access("f1", F_OK) != 0 && access("f_new", F_OK) == 0, 1);
    unlink("f_new");
    unlink("f2");

    /* 3. RENAME_EXCHANGE */
    create_file("ex1", "data_one");
    create_file("ex2", "data_two");
    record("exchange.files", do_renameat2(AT_FDCWD, "ex1", AT_FDCWD, "ex2", RENAME_EXCHANGE), 0);
    char buf1[32], buf2[32];
    read_file("ex1", buf1, sizeof(buf1));
    read_file("ex2", buf2, sizeof(buf2));
    record("exchange.files_contents", strcmp(buf1, "data_two") == 0 && strcmp(buf2, "data_one") == 0, 1);
    unlink("ex1");
    unlink("ex2");

    /* Exchange directories */
    if (mkdir("d1", 0755) != 0) fatal("mkdir d1");
    if (mkdir("d2", 0755) != 0) fatal("mkdir d2");
    create_file("d1/sub1", "sub_one");
    create_file("d2/sub2", "sub_two");
    record("exchange.directories", do_renameat2(AT_FDCWD, "d1", AT_FDCWD, "d2", RENAME_EXCHANGE), 0);
    record("exchange.directories_children", access("d1/sub2", F_OK) == 0 && access("d2/sub1", F_OK) == 0, 1);
    unlink("d1/sub2");
    unlink("d2/sub1");
    rmdir("d1");
    rmdir("d2");

    /* Exchange file and directory */
    create_file("ex_f", "file_data");
    if (mkdir("ex_d", 0755) != 0) fatal("mkdir ex_d");
    create_file("ex_d/child", "inside_dir");
    record("exchange.file_and_directory", do_renameat2(AT_FDCWD, "ex_f", AT_FDCWD, "ex_d", RENAME_EXCHANGE), 0);
    record("exchange.file_and_directory_types", is_dir("ex_f") && !is_dir("ex_d"), 1);
    record("exchange.file_and_directory_contents", access("ex_f/child", F_OK) == 0, 1);
    unlink("ex_f/child");
    rmdir("ex_f");
    unlink("ex_d");

    /* Exchange file and symlink */
    create_file("ex_rf", "real_file");
    if (symlink("ex_rf", "ex_sl") != 0) fatal("symlink");
    record("exchange.file_and_symlink", do_renameat2(AT_FDCWD, "ex_rf", AT_FDCWD, "ex_sl", RENAME_EXCHANGE), 0);
    record("exchange.file_and_symlink_types", is_link("ex_rf") && !is_link("ex_sl"), 1);
    unlink("ex_rf");
    unlink("ex_sl");

    /* Exchange missing / same / descendant */
    create_file("ex_exist", "x");
    record("exchange.missing_source", do_renameat2(AT_FDCWD, "no_such_src", AT_FDCWD, "ex_exist", RENAME_EXCHANGE), -ENOENT);
    record("exchange.missing_target", do_renameat2(AT_FDCWD, "ex_exist", AT_FDCWD, "no_such_tgt", RENAME_EXCHANGE), -ENOENT);
    record("exchange.same_file", do_renameat2(AT_FDCWD, "ex_exist", AT_FDCWD, "ex_exist", RENAME_EXCHANGE), 0);
    unlink("ex_exist");

    if (mkdir("p_dir", 0755) != 0) fatal("mkdir p_dir");
    if (mkdir("p_dir/c_dir", 0755) != 0) fatal("mkdir c_dir");
    record("exchange.descendant_parent_to_child", do_renameat2(AT_FDCWD, "p_dir", AT_FDCWD, "p_dir/c_dir", RENAME_EXCHANGE), -EINVAL);
    record("exchange.descendant_child_to_parent", do_renameat2(AT_FDCWD, "p_dir/c_dir", AT_FDCWD, "p_dir", RENAME_EXCHANGE), -EINVAL);
    rmdir("p_dir/c_dir");
    rmdir("p_dir");

    /* 4. Type mismatch (without RENAME_EXCHANGE) */
    create_file("test_f", "reg");
    if (mkdir("test_d", 0755) != 0) fatal("mkdir test_d");
    record("type_mismatch.dir_onto_file", do_renameat2(AT_FDCWD, "test_d", AT_FDCWD, "test_f", 0), -ENOTDIR);
    record("type_mismatch.file_onto_dir", do_renameat2(AT_FDCWD, "test_f", AT_FDCWD, "test_d", 0), -EISDIR);

    if (mkdir("nonempty_d", 0755) != 0) fatal("mkdir nonempty_d");
    create_file("nonempty_d/child", "c");
    record("type_mismatch.dir_onto_nonempty_dir", do_renameat2(AT_FDCWD, "test_d", AT_FDCWD, "nonempty_d", 0), -ENOTEMPTY);
    unlink("nonempty_d/child");
    record("type_mismatch.dir_onto_empty_dir", do_renameat2(AT_FDCWD, "test_d", AT_FDCWD, "nonempty_d", 0), 0);
    record("type_mismatch.empty_dir_replaced", access("test_d", F_OK) != 0 && is_dir("nonempty_d"), 1);
    rmdir("nonempty_d");
    unlink("test_f");

    /* 5. Trailing slashes */
    create_file("slash_f", "data");
    record("trailing_slash.file_with_slash_source", do_renameat2(AT_FDCWD, "slash_f/", AT_FDCWD, "slash_target", 0), -ENOTDIR);
    record("trailing_slash.file_with_slash_target", do_renameat2(AT_FDCWD, "slash_f", AT_FDCWD, "missing_d/", 0), -ENOTDIR);
    create_file("target_f", "tgt");
    record("trailing_slash.file_onto_file_with_slash_target", do_renameat2(AT_FDCWD, "slash_f", AT_FDCWD, "target_f/", 0), -ENOTDIR);
    unlink("target_f");

    if (symlink("slash_f", "sym_to_file") != 0) fatal("symlink");
    record("trailing_slash.symlink_to_file_with_slash", do_renameat2(AT_FDCWD, "sym_to_file/", AT_FDCWD, "dest", 0), -ENOTDIR);
    unlink("sym_to_file");
    unlink("slash_f");

    if (mkdir("target_d", 0755) != 0) fatal("mkdir target_d");
    if (symlink("target_d", "sym_to_dir") != 0) fatal("symlink");
    record("trailing_slash.symlink_to_dir_with_slash", do_renameat2(AT_FDCWD, "sym_to_dir/", AT_FDCWD, "dest", 0), -ENOTDIR);
    unlink("sym_to_dir");

    record("trailing_slash.dir_with_slash_source", do_renameat2(AT_FDCWD, "target_d/", AT_FDCWD, "target_d2", 0), 0);
    record("trailing_slash.dir_with_slash_target", do_renameat2(AT_FDCWD, "target_d2", AT_FDCWD, "target_d3/", 0), 0);
    rmdir("target_d3");

    /* 6. Special names */
    create_file("spec_src", "s");
    record("special.dot_source", do_renameat2(AT_FDCWD, ".", AT_FDCWD, "spec_tgt", 0), -EBUSY);
    record("special.dotdot_source", do_renameat2(AT_FDCWD, "..", AT_FDCWD, "spec_tgt", 0), -EBUSY);
    record("special.dot_target", do_renameat2(AT_FDCWD, "spec_src", AT_FDCWD, ".", 0), -EBUSY);
    record("special.dotdot_target", do_renameat2(AT_FDCWD, "spec_src", AT_FDCWD, "..", 0), -EBUSY);
    unlink("spec_src");

    /* 7. Empty path & pointer errors */
    record("empty.source", do_renameat2(AT_FDCWD, "", AT_FDCWD, "target", 0), -ENOENT);
    create_file("valid_src", "v");
    record("empty.target", do_renameat2(AT_FDCWD, "valid_src", AT_FDCWD, "", 0), -ENOENT);
    record("pointer.null_source", do_renameat2(AT_FDCWD, NULL, AT_FDCWD, "target", 0), -EFAULT);
    record("pointer.null_target", do_renameat2(AT_FDCWD, "valid_src", AT_FDCWD, NULL, 0), -EFAULT);
    record("pointer.bad_source", do_renameat2(AT_FDCWD, (const char *)1, AT_FDCWD, "target", 0), -EFAULT);
    record("pointer.bad_target", do_renameat2(AT_FDCWD, "valid_src", AT_FDCWD, (const char *)1, 0), -EFAULT);

    /* 8. Dirfd errors */
    record("dirfd.bad_source", do_renameat2(-1, "valid_src", AT_FDCWD, "target", 0), -EBADF);
    record("dirfd.bad_target", do_renameat2(AT_FDCWD, "valid_src", -1, "target", 0), -EBADF);

    int file_fd = open("valid_src", O_RDONLY);
    if (file_fd < 0) fatal("open valid_src");
    record("dirfd.file_as_source", do_renameat2(file_fd, "valid_src", AT_FDCWD, "target", 0), -ENOTDIR);
    record("dirfd.file_as_target", do_renameat2(AT_FDCWD, "valid_src", file_fd, "target", 0), -ENOTDIR);
    close(file_fd);
    unlink("valid_src");

    /* 9. Symlink behavior */
    if (symlink("dead_target", "dead_link") != 0) fatal("symlink dead");
    record("symlink.dead_source", do_renameat2(AT_FDCWD, "dead_link", AT_FDCWD, "moved_dead", 0), 0);
    record("symlink.dead_source_verified", is_link("moved_dead") && access("dead_link", F_OK) != 0, 1);

    create_file("real_src", "hello");
    record("symlink.replace_dead_target", do_renameat2(AT_FDCWD, "real_src", AT_FDCWD, "moved_dead", 0), 0);
    record("symlink.replace_dead_verified", !is_link("moved_dead") && access("moved_dead", F_OK) == 0, 1);
    unlink("moved_dead");

    if (symlink("loop_link", "loop_link") != 0) fatal("symlink loop");
    record("symlink.intermediate_loop", do_renameat2(AT_FDCWD, "loop_link/file", AT_FDCWD, "dest", 0), -ELOOP);
    unlink("loop_link");

    /* 10. Same file no-op */
    create_file("same_f", "content");
    record("same.existing_file", do_renameat2(AT_FDCWD, "same_f", AT_FDCWD, "same_f", 0), 0);
    record("same.nonexistent_file", do_renameat2(AT_FDCWD, "no_such_file", AT_FDCWD, "no_such_file", 0), -ENOENT);
    unlink("same_f");

    /* 11. Legacy rename and renameat wrappers */
    create_file("leg_src", "legacy");
    record("legacy.rename_call", do_rename("leg_src", "leg_dst"), 0);
    record("legacy.renameat_call", do_renameat(AT_FDCWD, "leg_dst", AT_FDCWD, "leg_at"), 0);
    unlink("leg_at");

    /* 12. Concurrent race stress */
    run_race_test();

    /* Cleanup testdir */
    chdir("/tmp");
    rmdir(testdir);

    puts("== namespaces skipped: 0 ==");
    printf("== probe complete: %d passed, %d failed ==\n", passed, failed);
    return failed != 0;
}
