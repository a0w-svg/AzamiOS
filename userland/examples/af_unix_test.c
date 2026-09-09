/* ============================================================================
 * AzamiOS — AF_UNIX + real sendmsg/recvmsg regression test
 * File: userland/examples/af_unix_test.c
 *
 * Exercises the Phase 2 socket work: a real AF_UNIX SOCK_STREAM
 * bind/listen/connect/accept handshake, an AF_UNIX SOCK_DGRAM exchange that
 * passes an open file descriptor over SCM_RIGHTS, and multi-iovec
 * sendmsg()/recvmsg() gather/scatter over a UDP loopback pair (the part of
 * "real sendmsg/recvmsg" that applies to every socket domain, not just
 * AF_UNIX — SYS_sendmsg/SYS_recvmsg used to be literal aliases for
 * sendto/recvfrom, silently dropping every iovec past the first).
 * ============================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("[PASS] %s\n", msg); \
    else { printf("[FAIL] %s (errno=%d)\n", msg, errno); g_fail++; } \
} while (0)

static void test_unix_stream(void)
{
    printf("--- AF_UNIX SOCK_STREAM (bind/listen/connect/accept) ---\n");
    const char *path = "/tmp/af_unix_stream_test.sock";
    unlink(path);

    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(lfd >= 0, "socket(AF_UNIX, SOCK_STREAM)");

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    CHECK(bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) == 0, "bind() to a path");
    CHECK(listen(lfd, 4) == 0, "listen()");

    pid_t pid = fork();
    if (pid < 0) { CHECK(0, "fork()"); return; }
    if (pid == 0) {
        /* Child: connect and exchange a two-iovec message. */
        int cfd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (cfd < 0) _exit(1);
        if (connect(cfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) _exit(2);

        const char *part1 = "Hello, ";
        const char *part2 = "AF_UNIX stream!";
        struct iovec iov[2] = {
            { (void *)part1, strlen(part1) },
            { (void *)part2, strlen(part2) },
        };
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = iov;
        msg.msg_iovlen = 2;
        ssize_t n = sendmsg(cfd, &msg, 0);
        if (n != (ssize_t)(strlen(part1) + strlen(part2))) _exit(3);

        char reply[64] = {0};
        ssize_t rn = recv(cfd, reply, sizeof(reply) - 1, 0);
        if (rn <= 0 || strcmp(reply, "ack") != 0) _exit(4);
        close(cfd);
        _exit(0);
    }

    /* Parent: accept, receive the gathered multi-iovec message, verify it
     * was reassembled correctly, then reply. */
    CHECK(1, "fork()");
    int afd = accept(lfd, NULL, NULL);
    CHECK(afd >= 0, "accept()");

    char buf1[16] = {0}, buf2[32] = {0};
    struct iovec riov[2] = {
        { buf1, sizeof(buf1) - 1 },
        { buf2, sizeof(buf2) - 1 },
    };
    struct msghdr rmsg;
    memset(&rmsg, 0, sizeof(rmsg));
    rmsg.msg_iov = riov;
    rmsg.msg_iovlen = 2;
    ssize_t rn = recvmsg(afd, &rmsg, 0);
    CHECK(rn > 0, "recvmsg() on accepted socket");

    char combined[64];
    snprintf(combined, sizeof(combined), "%s%s", buf1, buf2);
    CHECK(strcmp(combined, "Hello, AF_UNIX stream!") == 0,
          "recvmsg() scattered a multi-iovec send back correctly");

    CHECK(send(afd, "ack", 3, 0) == 3, "send() ack to child");

    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child completed its side without error");

    close(afd);
    close(lfd);
    unlink(path);
}

static void test_unix_dgram_scm_rights(void)
{
    printf("--- AF_UNIX SOCK_DGRAM + SCM_RIGHTS fd-passing ---\n");
    const char *path_a = "/tmp/af_unix_dgram_a.sock";
    const char *path_b = "/tmp/af_unix_dgram_b.sock";
    unlink(path_a);
    unlink(path_b);

    int fa = socket(AF_UNIX, SOCK_DGRAM, 0);
    int fb = socket(AF_UNIX, SOCK_DGRAM, 0);
    CHECK(fa >= 0 && fb >= 0, "socket(AF_UNIX, SOCK_DGRAM) x2");

    struct sockaddr_un aa, ab;
    memset(&aa, 0, sizeof(aa)); aa.sun_family = AF_UNIX; strncpy(aa.sun_path, path_a, sizeof(aa.sun_path) - 1);
    memset(&ab, 0, sizeof(ab)); ab.sun_family = AF_UNIX; strncpy(ab.sun_path, path_b, sizeof(ab.sun_path) - 1);

    CHECK(bind(fa, (struct sockaddr *)&aa, sizeof(aa)) == 0, "bind A");
    CHECK(bind(fb, (struct sockaddr *)&ab, sizeof(ab)) == 0, "bind B");

    /* Open a real file with known contents, and pass its fd from A to B. */
    const char *tmpfile = "/tmp/af_unix_scm_rights_payload.txt";
    int wfd = open(tmpfile, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    CHECK(wfd >= 0, "create payload file");
    write(wfd, "passed-fd-payload", 18);
    close(wfd);
    int payload_fd = open(tmpfile, O_RDONLY);
    CHECK(payload_fd >= 0, "open payload file to pass");

    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr smsg;
    memset(&smsg, 0, sizeof(smsg));
    smsg.msg_name = &ab;
    smsg.msg_namelen = sizeof(ab);
    const char *hello = "here's an fd";
    struct iovec siov = { (void *)hello, strlen(hello) };
    smsg.msg_iov = &siov;
    smsg.msg_iovlen = 1;
    smsg.msg_control = cbuf;
    smsg.msg_controllen = sizeof(cbuf);

    struct cmsghdr *scm = CMSG_FIRSTHDR(&smsg);
    scm->cmsg_level = SOL_SOCKET;
    scm->cmsg_type = SCM_RIGHTS;
    scm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(scm), &payload_fd, sizeof(int));

    ssize_t sn = sendmsg(fa, &smsg, 0);
    CHECK(sn == (ssize_t)strlen(hello), "sendmsg() with SCM_RIGHTS");
    close(payload_fd); /* sender's own copy — the receiver gets its own dup */

    char rbuf[64] = {0};
    char rcbuf[CMSG_SPACE(sizeof(int))];
    struct iovec riov = { rbuf, sizeof(rbuf) - 1 };
    struct msghdr rmsg;
    memset(&rmsg, 0, sizeof(rmsg));
    rmsg.msg_iov = &riov;
    rmsg.msg_iovlen = 1;
    rmsg.msg_control = rcbuf;
    rmsg.msg_controllen = sizeof(rcbuf);

    ssize_t rn = recvmsg(fb, &rmsg, 0);
    CHECK(rn == (ssize_t)strlen(hello) && strcmp(rbuf, hello) == 0, "recvmsg() payload matches");

    struct cmsghdr *rcm = CMSG_FIRSTHDR(&rmsg);
    CHECK(rcm != NULL && rcm->cmsg_type == SCM_RIGHTS, "recvmsg() carried an SCM_RIGHTS cmsg");

    if (rcm && rcm->cmsg_type == SCM_RIGHTS) {
        int received_fd = -1;
        memcpy(&received_fd, CMSG_DATA(rcm), sizeof(int));
        /* Not compared against payload_fd: both sockets are in this same
         * process, and payload_fd was already close()d above, so fd-table
         * reuse can legitimately hand the received fd that same number
         * back — that's correct behavior, not a sign it's the same
         * descriptor. The read-back check below is what actually proves
         * this is a live, independent fd. */
        CHECK(received_fd >= 0, "received fd is a valid descriptor");

        char verify[32] = {0};
        ssize_t vn = read(received_fd, verify, sizeof(verify) - 1);
        CHECK(vn == 18 && strcmp(verify, "passed-fd-payload") == 0,
              "the passed fd actually reads the sender's file contents");
        close(received_fd);
    }

    close(fa);
    close(fb);
    unlink(path_a);
    unlink(path_b);
    unlink(tmpfile);
}

static void test_udp_multi_iovec(void)
{
    printf("--- Multi-iovec sendmsg/recvmsg over UDP loopback ---\n");
    int sfd = socket(AF_INET, SOCK_DGRAM, 0);
    int rfd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(sfd >= 0 && rfd >= 0, "socket(AF_INET, SOCK_DGRAM) x2");

    struct sockaddr_in raddr;
    memset(&raddr, 0, sizeof(raddr));
    raddr.sin_family = AF_INET;
    raddr.sin_port = htons(38121);
    raddr.sin_addr.s_addr = htonl(0x7F000001); /* 127.0.0.1 */
    CHECK(bind(rfd, (struct sockaddr *)&raddr, sizeof(raddr)) == 0, "bind receiver to 127.0.0.1:38121");

    const char *a = "multi-";
    const char *b = "iovec-";
    const char *c = "udp";
    struct iovec siov[3] = {
        { (void *)a, strlen(a) },
        { (void *)b, strlen(b) },
        { (void *)c, strlen(c) },
    };
    struct msghdr smsg;
    memset(&smsg, 0, sizeof(smsg));
    smsg.msg_name = &raddr;
    smsg.msg_namelen = sizeof(raddr);
    smsg.msg_iov = siov;
    smsg.msg_iovlen = 3;

    ssize_t sn = sendmsg(sfd, &smsg, 0);
    CHECK(sn == (ssize_t)(strlen(a) + strlen(b) + strlen(c)), "sendmsg() gathers all 3 iovecs");

    char p1[8] = {0}, p2[8] = {0}, p3[8] = {0};
    struct iovec riov[3] = {
        { p1, sizeof(p1) - 1 },
        { p2, sizeof(p2) - 1 },
        { p3, sizeof(p3) - 1 },
    };
    struct msghdr rmsg;
    memset(&rmsg, 0, sizeof(rmsg));
    rmsg.msg_iov = riov;
    rmsg.msg_iovlen = 3;
    ssize_t rn = recvmsg(rfd, &rmsg, 0);
    CHECK(rn > 0, "recvmsg() on UDP socket");

    char combined[32];
    snprintf(combined, sizeof(combined), "%s%s%s", p1, p2, p3);
    CHECK(strcmp(combined, "multi-iovec-udp") == 0, "recvmsg() scattered all 3 iovecs correctly");

    close(sfd);
    close(rfd);
}

int main(void)
{
    printf("=== AzamiOS AF_UNIX + sendmsg/recvmsg test ===\n");
    test_unix_stream();
    test_unix_dgram_scm_rights();
    test_udp_multi_iovec();

    if (g_fail == 0) {
        printf("=== ALL PASS ===\n");
        return 0;
    }
    printf("=== %d FAILURE(S) ===\n", g_fail);
    return 1;
}
