/* Host regression test using deterministic fd operations.
 * Run with: sh tools/tests/stdio-buffer-test.sh
 */
#include "../../userland/libc/stdio.c"

__thread int errno;
static const char input[] = "alpha\nbeta\ngamma";
static long position;
static int reads;
static int fail_seek;

ssize_t sys_read(int fd, void *buf, size_t len)
{
    (void)fd;
    reads++;
    size_t available = sizeof(input) - 1 - (size_t)position;
    if (len > available) len = available;
    memcpy(buf, input + position, len);
    position += (long)len;
    return (ssize_t)len;
}

ssize_t sys_lseek(int fd, ssize_t offset, int whence)
{
    (void)fd;
    if (fail_seek) { errno = ESPIPE; return -1; }
    long next = whence == SEEK_CUR ? position + offset : offset;
    if (next < 0 || next >= (long)sizeof(input)) {
        errno = EINVAL;
        return -1;
    }
    position = next;
    return position;
}

#define CHECK(expr) do { if (!(expr)) return __LINE__; } while (0)

int main(void)
{
    FILE stream = { .fd = 3, .unget_char = -1 };
    char line[32];
    CHECK(fgets(line, sizeof(line), &stream) == line);
    CHECK(strcmp(line, "alpha\n") == 0 && reads == 1);
    CHECK(ftell(&stream) == 6);
    CHECK(fgetc(&stream) == 'b');
    CHECK(ungetc('B', &stream) == 'B');
    CHECK(fgets(line, sizeof(line), &stream) == line);
    CHECK(strcmp(line, "Beta\n") == 0 && reads == 1);
    CHECK(ftell(&stream) == 11);
    CHECK(fseek(&stream, -5, SEEK_CUR) == 0);
    CHECK(fread(line, 1, 5, &stream) == 5);
    CHECK(memcmp(line, "beta\n", 5) == 0);

    CHECK(fseek(&stream, 0, SEEK_SET) == 0);
    CHECK(fgetc(&stream) == 'a');
    fail_seek = 1;
    CHECK(fseek(&stream, 0, SEEK_SET) == -1 && errno == ESPIPE);
    CHECK(fgetc(&stream) == 'l'); /* Failed seek preserves unread data. */
    fail_seek = 0;
    CHECK(fseek(&stream, (-__LONG_MAX__ - 1), SEEK_CUR) == -1);
    CHECK(errno == EOVERFLOW && fgetc(&stream) == 'p');
    CHECK(fseek(&stream, 11, SEEK_SET) == 0);
    CHECK(fgets(line, sizeof(line), &stream) == line);
    CHECK(strcmp(line, "gamma") == 0 && stream.eof);
    CHECK(fseek(&stream, 0, SEEK_SET) == 0 && !stream.eof);
    CHECK(fgets(line, 1, &stream) == line && line[0] == 0);

    FILE memory = { .is_memstream = 1, .mem_buf = "one\ntwo",
                    .mem_size = 7, .unget_char = -1 };
    CHECK(fgets(line, sizeof(line), &memory) == line);
    CHECK(strcmp(line, "one\n") == 0 && memory.mem_pos == 4);
    position = 0;
    CHECK(fgets(line, sizeof(line), stdin) == line);
    CHECK(strcmp(line, "alpha\n") == 0);
    return 0;
}
