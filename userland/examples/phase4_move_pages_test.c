/* ============================================================================
 * AzamiOS — Phase 4 move_pages(2) regression test
 * File: userland/examples/phase4_move_pages_test.c
 * ============================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <numaif.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("[PASS] %s\n", msg); \
    else { printf("[FAIL] %s (errno=%d)\n", msg, errno); g_fail++; } \
} while (0)

int main(void)
{
    printf("=== AzamiOS Phase 4 move_pages test ===\n");

    void *page = malloc(4096);
    CHECK(page != NULL, "malloc a page-sized buffer");
    /* Touch it so it's actually mapped, not just reserved. */
    *(volatile char *)page = 1;

    void *pages[2] = { page, (void *)0x1 /* deliberately unmapped */ };
    int status[2] = { -99, -99 };

    long r = move_pages(0, 2, pages, NULL, status, 0);
    CHECK(r == 0, "move_pages() query call succeeds");
    CHECK(status[0] == 0, "present page reports node 0");
    CHECK(status[1] == -ENOENT, "unmapped page reports -ENOENT");

    int nodes_ok[2] = { 0, 0 };
    int status2[2] = { -99, -99 };
    r = move_pages(0, 2, pages, nodes_ok, status2, 0);
    CHECK(r == 0, "move_pages() request-to-node-0 call succeeds");
    CHECK(status2[0] == 0, "requesting node 0 for a present page succeeds");
    CHECK(status2[1] == -ENOENT, "requesting a move for an unmapped page still reports -ENOENT");

    int nodes_bad[1] = { 5 };
    int status3[1] = { -99 };
    void *pages_one[1] = { page };
    r = move_pages(0, 1, pages_one, nodes_bad, status3, 0);
    CHECK(r == 0, "move_pages() call itself succeeds even if a page's move fails");
    CHECK(status3[0] == -ENODEV, "requesting a nonexistent node reports -ENODEV");

    /* pid != self must be rejected. */
    errno = 0;
    r = move_pages(1, 1, pages_one, NULL, status3, 0);
    CHECK(r == -1 && errno == EPERM, "move_pages() on another pid is rejected with EPERM");

    free(page);

    if (g_fail == 0) {
        printf("=== ALL PASS ===\n");
        return 0;
    }
    printf("=== %d FAILURE(S) ===\n", g_fail);
    return 1;
}
