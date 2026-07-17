/**
 * kernel/mem/aslr.c — Address Space Layout Randomization (ASLR) for AzamiOS v6.0
 */
#include "include/security.h"

static uintptr_t s_aslr_seed = 0x1A2B3C4DU;
static uintptr_t s_aslr_offset = 0;

void aslr_init(void) {
    /* Seed ASLR offset */
    s_aslr_seed ^= (s_aslr_seed << 13);
    s_aslr_seed ^= (s_aslr_seed >> 17);
    s_aslr_seed ^= (s_aslr_seed << 5);
    s_aslr_offset = (s_aslr_seed & 0xFFF) * 0x1000; /* Page-aligned random offset up to 16MB */
}

uintptr_t aslr_randomize_base(uintptr_t base_addr, size_t alignment) {
    s_aslr_seed ^= (s_aslr_seed << 13);
    s_aslr_seed ^= (s_aslr_seed >> 17);
    s_aslr_seed ^= (s_aslr_seed << 5);

    uintptr_t rand_pages = (s_aslr_seed & 0x3FF) * 0x1000;
    if (alignment > 0) {
        rand_pages = (rand_pages / alignment) * alignment;
    }
    return base_addr + rand_pages;
}

uintptr_t aslr_get_offset(void) {
    return s_aslr_offset;
}
