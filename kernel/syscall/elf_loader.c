/**
 * elf_loader.c  –  ELF loader for AzamiOS
 *
 * Validates an ELF64 executable, allocates physical frames
 * for every PT_LOAD segment, maps them into the virtual address space (user-mode, writable),
 * copies the file image in, and zero-fills any BSS tail.
 */

#include "include/elf_loader.h"
#include "../filesystem/include/vfs.h"
#include "../mem/include/paging.h"
#include "../mem/include/pmm.h"
#include "../klibc/include/string.h"
#include "../klibc/include/stdio.h"

/* ─── helpers ────────────────────────────────────────────────────────────── */

static inline uintptr_t align_up(uintptr_t v, uintptr_t align) {
    return (v + align - 1) & ~(align - 1);
}

static int read_exact(fs_node_t *node, uint32_t offset,
                      uint32_t size, uint8_t *buf) {
    block_device_t dummy = {0};
    uint32_t got = node->read(&dummy, node, offset, size, buf);
    return (got == size) ? ELF_LOAD_OK : ELF_LOAD_ERR_READ;
}

/* ─── header validation ──────────────────────────────────────────────────── */

static int validate_ehdr(const Elf_Ehdr_t *hdr) {
    uint32_t magic =
        (uint32_t)hdr->e_ident[0]        |
        ((uint32_t)hdr->e_ident[1] <<  8) |
        ((uint32_t)hdr->e_ident[2] << 16) |
        ((uint32_t)hdr->e_ident[3] << 24);

    if (magic != ELF_MAGIC) {
        kprintf("elf: bad magic 0x%x\n", magic);
        return ELF_LOAD_ERR_NOT_ELF;
    }

    if (hdr->e_ident[EI_CLASS] != ELF_TARGET_CLASS) {
        kprintf("elf: unsupported class %d (expected %d)\n", hdr->e_ident[EI_CLASS], ELF_TARGET_CLASS);
        return ELF_LOAD_ERR_NOT_32;
    }

    if (hdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        kprintf("elf: not little-endian (data=%d)\n", hdr->e_ident[EI_DATA]);
        return ELF_LOAD_ERR_NOT_LSB;
    }

    if (hdr->e_type != ET_EXEC && hdr->e_type != ET_DYN) {
        kprintf("elf: not an executable or Linux PIE (type=%d)\n", hdr->e_type);
        return ELF_LOAD_ERR_NOT_EXEC;
    }

    if (hdr->e_machine != ELF_TARGET_MACHINE) {
        kprintf("elf: wrong architecture (machine=%d, expected %d)\n", hdr->e_machine, ELF_TARGET_MACHINE);
        return ELF_LOAD_ERR_WRONG_ARCH;
    }

    if (hdr->e_version != EV_CURRENT) {
        kprintf("elf: unsupported version (%d)\n", hdr->e_version);
        return ELF_LOAD_ERR_BAD_VER;
    }

    if (hdr->e_phnum == 0 || hdr->e_phoff == 0) {
        kprintf("elf: no program headers\n");
        return ELF_LOAD_ERR_NO_PHDR;
    }

    return ELF_LOAD_OK;
}

/* ─── segment loader ─────────────────────────────────────────────────────── */

static int load_segment(fs_node_t *node, const Elf_Phdr_t *phdr) {
    if (phdr->p_memsz == 0) {
        return ELF_LOAD_OK;
    }

    uintptr_t vaddr_base   = phdr->p_vaddr & ~(PAGE_SIZE - 1);
    uintptr_t vaddr_end    = align_up(phdr->p_vaddr + phdr->p_memsz, PAGE_SIZE);
    uint32_t page_count    = (vaddr_end - vaddr_base) / PAGE_SIZE;

    uint8_t writable = 1;

    for (uint32_t i = 0; i < page_count; i++) {
        uintptr_t virt = vaddr_base + i * PAGE_SIZE;

        void *phys = pmm_alloc_block();
        if (!phys) {
            kprintf("elf: out of physical memory at vaddr=0x%x\n", (uint32_t)virt);
            return ELF_LOAD_ERR_NO_MEM;
        }

        paging_map_page((uintptr_t)phys, virt, 0 /* user */, writable);
        memset((void *)virt, 0, PAGE_SIZE);
    }

    if (phdr->p_filesz > 0) {
        int rc = read_exact(node, (uint32_t)phdr->p_offset,
                            (uint32_t)phdr->p_filesz, (uint8_t *)(uintptr_t)phdr->p_vaddr);
        if (rc != ELF_LOAD_OK) {
            kprintf("elf: failed to read segment at offset 0x%x\n",
                    (uint32_t)phdr->p_offset);
            return rc;
        }
    }

    if (phdr->p_memsz > phdr->p_filesz) {
        uint8_t  *bss   = (uint8_t *)(uintptr_t)(phdr->p_vaddr + phdr->p_filesz);
        uint32_t  bsssz = (uint32_t)(phdr->p_memsz - phdr->p_filesz);
        memset(bss, 0, bsssz);
    }

    return ELF_LOAD_OK;
}

/* ─── public entry point ─────────────────────────────────────────────────── */

int load_elf(fs_node_t *file_node, uintptr_t *entry_out) {
    Elf_Ehdr_t ehdr;
    int rc;

    rc = read_exact(file_node, 0, sizeof(Elf_Ehdr_t), (uint8_t *)&ehdr);
    if (rc != ELF_LOAD_OK) {
        kprintf("elf: could not read ELF header\n");
        return rc;
    }

    rc = validate_ehdr(&ehdr);
    if (rc != ELF_LOAD_OK) {
        return rc;
    }

    kprintf("elf: loading executable, entry=0x%llx, phnum=%d\n",
            (unsigned long long)ehdr.e_entry, ehdr.e_phnum);

    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        Elf_Phdr_t phdr;
        uint32_t phdr_offset = (uint32_t)ehdr.e_phoff + (uint32_t)(i * ehdr.e_phentsize);

        rc = read_exact(file_node, phdr_offset,
                        sizeof(Elf_Phdr_t), (uint8_t *)&phdr);
        if (rc != ELF_LOAD_OK) {
            kprintf("elf: failed to read program header %d\n", i);
            return rc;
        }

        if (phdr.p_type != PT_LOAD) {
            continue;
        }

        rc = load_segment(file_node, &phdr);
        if (rc != ELF_LOAD_OK) {
            return rc;
        }
    }

    *entry_out = (uintptr_t)ehdr.e_entry;
    kprintf("elf: load complete, entry point = 0x%llx\n", (unsigned long long)ehdr.e_entry);
    return ELF_LOAD_OK;
}

