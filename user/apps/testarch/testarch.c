/*
 * testarch.c — AzamiOS Architecture Verification Utility
 *
 * Verifies bitness, pointer sizes, data type widths, syscall ABI, and memory safety
 * for x86_64 (64-bit) mode.
 * Writes formatted results to cmd_out for display in AzamiOS Shell/Terminal.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>

void _start(void) {
    char out_buf[2048];
    int offset = 0;

    offset += snprintf(out_buf + offset, sizeof(out_buf) - offset,
        "====================================================\n"
        "     AzamiOS Architecture Verification Suite\n"
        "====================================================\n\n");

#if defined(__x86_64__)
    offset += snprintf(out_buf + offset, sizeof(out_buf) - offset, "[ARCH] Target Architecture : x86_64 (64-bit)\n");
    int expected_ptr_size = 8;
#else
    offset += snprintf(out_buf + offset, sizeof(out_buf) - offset, "[ARCH] Target Architecture : Unknown (Expected x86_64)\n");
    int expected_ptr_size = 8;
#endif

    int pass_count = 0;
    int total_tests = 0;

    /* Test 1: Pointer Width Verification */
    total_tests++;
    offset += snprintf(out_buf + offset, sizeof(out_buf) - offset, "[TEST 1] Pointer Width Verification... ");
    if (sizeof(void*) == (size_t)expected_ptr_size && sizeof(uintptr_t) == (size_t)expected_ptr_size) {
        offset += snprintf(out_buf + offset, sizeof(out_buf) - offset, "PASS (%u bytes)\n", (unsigned)sizeof(void*));
        pass_count++;
    } else {
        offset += snprintf(out_buf + offset, sizeof(out_buf) - offset, "FAIL! Expected %d bytes, got %u bytes\n", expected_ptr_size, (unsigned)sizeof(void*));
    }

    /* Test 2: Standard Data Type Widths */
    total_tests++;
    offset += snprintf(out_buf + offset, sizeof(out_buf) - offset, "[TEST 2] Data Type Widths... ");
    if (sizeof(char) == 1 && sizeof(short) == 2 && sizeof(int) == 4 && sizeof(long long) == 8) {
        offset += snprintf(out_buf + offset, sizeof(out_buf) - offset, "PASS (char=1, short=2, int=4, long long=8)\n");
        pass_count++;
    } else {
        offset += snprintf(out_buf + offset, sizeof(out_buf) - offset, "FAIL! Unexpected data type sizing.\n");
    }

    /* Test 3: Syscall ABI Sanity (getpid) */
    total_tests++;
    offset += snprintf(out_buf + offset, sizeof(out_buf) - offset, "[TEST 3] Syscall ABI Sanity... ");
    int pid = getpid();
    if (pid > 0) {
        offset += snprintf(out_buf + offset, sizeof(out_buf) - offset, "PASS (PID = %d)\n", pid);
        pass_count++;
    } else {
        offset += snprintf(out_buf + offset, sizeof(out_buf) - offset, "FAIL! Invalid PID returned (%d)\n", pid);
    }

    /* Test 4: Dynamic Memory Allocation & Pointer Alignment */
    total_tests++;
    offset += snprintf(out_buf + offset, sizeof(out_buf) - offset, "[TEST 4] Dynamic Memory Allocation... ");
    void *ptr = malloc(1024);
    if (ptr != NULL) {
        memset(ptr, 0xAA, 1024);
        unsigned char *cptr = (unsigned char *)ptr;
        if (cptr[0] == 0xAA && cptr[1023] == 0xAA) {
            offset += snprintf(out_buf + offset, sizeof(out_buf) - offset, "PASS (Allocated & Verified at 0x%llx)\n", (unsigned long long)(uintptr_t)ptr);
            pass_count++;
        } else {
            offset += snprintf(out_buf + offset, sizeof(out_buf) - offset, "FAIL! Memory corruption detected.\n");
        }
        free(ptr);
    } else {
        offset += snprintf(out_buf + offset, sizeof(out_buf) - offset, "FAIL! malloc(1024) returned NULL.\n");
    }

    offset += snprintf(out_buf + offset, sizeof(out_buf) - offset,
        "\n====================================================\n"
        "Summary: %d / %d tests passed.\n", pass_count, total_tests);
    if (pass_count == total_tests) {
        offset += snprintf(out_buf + offset, sizeof(out_buf) - offset,
            "STATUS : ALL ARCHITECTURE TESTS PASSED SUCCESSFULLY!\n");
    } else {
        offset += snprintf(out_buf + offset, sizeof(out_buf) - offset,
            "STATUS : ARCHITECTURE VERIFICATION FAILED.\n");
    }
    offset += snprintf(out_buf + offset, sizeof(out_buf) - offset,
        "====================================================\n");

    int fd = open("cmd_out", O_WRONLY | O_CREAT, 0);
    if (fd >= 0) {
        write(fd, out_buf, strlen(out_buf));
        close(fd);
    }

    exit((pass_count == total_tests) ? 0 : 1);
}
