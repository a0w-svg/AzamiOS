#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../../userland/libc/include/internal/elf_hash.h"

int main(void)
{
    /* Header, one ELF64 bloom word, two buckets, three chain entries. */
    uint32_t table[] = { 2, 3, 1, 5, 0, 0, 3, 5, 0x10, 0x11, 0x21 };
    assert(__elf_gnu_hash_count(table, sizeof(table)) == 6);
    assert(__elf_gnu_hash_count(table, 0) == 0);
    assert(__elf_gnu_hash_count(table, 15) == 0);
    assert(__elf_gnu_hash_count(table, 16) == 0);
    assert(__elf_gnu_hash_count(table, sizeof(table) - 4) == 0);
    table[10] &= ~1U;
    assert(__elf_gnu_hash_count(table, sizeof(table)) == 0);
    table[10] |= 1U;
    table[6] = 2;
    assert(__elf_gnu_hash_count(table, sizeof(table)) == 0);
    table[6] = table[7] = 0;
    assert(__elf_gnu_hash_count(table, sizeof(table)) == 3);
    table[7] = UINT32_MAX;
    assert(__elf_gnu_hash_count(table, sizeof(table)) == 0);
    table[0] = UINT32_MAX;
    assert(__elf_gnu_hash_count(table, sizeof(table)) == 0);
    table[0] = 2;
    table[2] = UINT32_MAX;
    assert(__elf_gnu_hash_count(table, sizeof(table)) == 0);
    table[2] = 0;
    assert(__elf_gnu_hash_count(table, sizeof(table)) == 0);
    puts("PASS: GNU hash symbol counts and malformed table bounds");
    return 0;
}
