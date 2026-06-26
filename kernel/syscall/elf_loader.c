/**
 * elf_loader.c  –  ELF32 loader for AzamiOS
 *
 * Validates an ELF32 executable, allocates physical frames for every PT_LOAD
 * segment, maps them into the virtual address space (user-mode, writable),
 * copies the file image in, and zero-fills any BSS tail.
 *
 * Caller receives the entry-point address and is responsible for the actual
 * transfer of control (ring switch, stack setup, etc.).
 */

#include "include/elf_loader.h"
#include "../filesystem/include/vfs.h"
#include "../mem/include/paging.h"
#include "../mem/include/pmm.h"
#include "../klibc/include/string.h"
#include "../klibc/include/stdio.h"

/* ─── helpers ────────────────────────────────────────────────────────────── */

/** Round @v up to the nearest multiple of @align (must be power of two). */
static inline uint32_t align_up(uint32_t v, uint32_t align) {
    return (v + align - 1) & ~(align - 1);
}

/**
 * read_exact – thin wrapper around the VFS read callback that returns
 * 0 on success or ELF_LOAD_ERR_READ if fewer bytes were delivered.
 */
static int read_exact(fs_node_t *node, uint32_t offset,
                      uint32_t size, uint8_t *buf) {
    block_device_t dummy = {0};
    uint32_t got = node->read(&dummy, node, offset, size, buf);
    return (got == size) ? ELF_LOAD_OK : ELF_LOAD_ERR_READ;
}

/* ─── header validation ──────────────────────────────────────────────────── */

static int validate_ehdr(const Elf32_Ehdr_t *hdr) {
    /* 1. Magic number (\x7fELF) */
    uint32_t magic =
        (uint32_t)hdr->e_ident[0]        |
        ((uint32_t)hdr->e_ident[1] <<  8) |
        ((uint32_t)hdr->e_ident[2] << 16) |
        ((uint32_t)hdr->e_ident[3] << 24);

    if (magic != ELF_MAGIC) {
        kprintf("elf: bad magic 0x%x\n", magic);
        return ELF_LOAD_ERR_NOT_ELF;
    }

    /* 2. 32-bit class */
    if (hdr->e_ident[EI_CLASS] != ELFCLASS32) {
        kprintf("elf: not an ELF32 file (class=%d)\n", hdr->e_ident[EI_CLASS]);
        return ELF_LOAD_ERR_NOT_32;
    }

    /* 3. Little-endian */
    if (hdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        kprintf("elf: not little-endian (data=%d)\n", hdr->e_ident[EI_DATA]);
        return ELF_LOAD_ERR_NOT_LSB;
    }

    /* 4. Executable type */
    if (hdr->e_type != ET_EXEC) {
        kprintf("elf: not an executable (type=%d)\n", hdr->e_type);
        return ELF_LOAD_ERR_NOT_EXEC;
    }

    /* 5. i386 architecture */
    if (hdr->e_machine != EM_386) {
        kprintf("elf: wrong architecture (machine=%d)\n", hdr->e_machine);
        return ELF_LOAD_ERR_WRONG_ARCH;
    }

    /* 6. ELF version */
    if (hdr->e_version != EV_CURRENT) {
        kprintf("elf: unsupported version (%d)\n", hdr->e_version);
        return ELF_LOAD_ERR_BAD_VER;
    }

    /* 7. At least one program header */
    if (hdr->e_phnum == 0 || hdr->e_phoff == 0) {
        kprintf("elf: no program headers\n");
        return ELF_LOAD_ERR_NO_PHDR;
    }

    return ELF_LOAD_OK;
}

/* ─── segment loader ─────────────────────────────────────────────────────── */

/**
 * load_segment – map and populate a single PT_LOAD segment.
 *
 * Steps:
 *   a. Align the virtual address range down/up to page boundaries.
 *   b. Allocate one physical frame per page and map it (user, writable).
 *   c. Read the file image into virtual memory.
 *   d. Zero-fill the BSS tail (p_memsz > p_filesz).
 */
static int load_segment(fs_node_t *node, const Elf32_Phdr_t *phdr) {
    if (phdr->p_memsz == 0) {
        return ELF_LOAD_OK;   /* nothing to map */
    }

    /* Page-aligned base and total byte span */
    uint32_t vaddr_base   = phdr->p_vaddr & ~(PAGE_SIZE - 1);
    uint32_t vaddr_end    = align_up(phdr->p_vaddr + phdr->p_memsz, PAGE_SIZE);
    uint32_t page_count   = (vaddr_end - vaddr_base) / PAGE_SIZE;

    kprintf("elf: PT_LOAD vaddr=0x%x  filesz=0x%x  memsz=0x%x  pages=%d\n",
            phdr->p_vaddr, phdr->p_filesz, phdr->p_memsz, page_count);

    /* Determine whether the segment needs write permission */
    uint8_t writable = (phdr->p_flags & PF_W) ? 1 : 1; /* always writable for load */

    /* Allocate and map physical pages */
    for (uint32_t i = 0; i < page_count; i++) {
        uint32_t virt = vaddr_base + i * PAGE_SIZE;

        void *phys = pmm_alloc_block();
        if (!phys) {
            kprintf("elf: out of physical memory at vaddr=0x%x\n", virt);
            return ELF_LOAD_ERR_NO_MEM;
        }

        /* Map: not kernel (user=1), writable */
        paging_map_page((uint32_t)phys, virt, 0 /* user */, writable);

        /* Zero the freshly mapped page so BSS regions are clean */
        memset((void *)virt, 0, PAGE_SIZE);
    }

    /* Copy file image into virtual memory */
    if (phdr->p_filesz > 0) {
        int rc = read_exact(node, phdr->p_offset,
                            phdr->p_filesz, (uint8_t *)phdr->p_vaddr);
        if (rc != ELF_LOAD_OK) {
            kprintf("elf: failed to read segment at offset 0x%x\n",
                    phdr->p_offset);
            return rc;
        }
    }

    /*
     * BSS region (p_memsz > p_filesz): the pages were already zeroed above,
     * but we do an explicit memset for clarity and correctness in case the
     * allocator ever returns dirty pages.
     */
    if (phdr->p_memsz > phdr->p_filesz) {
        uint8_t  *bss   = (uint8_t *)(phdr->p_vaddr + phdr->p_filesz);
        uint32_t  bsssz = phdr->p_memsz - phdr->p_filesz;
        memset(bss, 0, bsssz);
    }

    return ELF_LOAD_OK;
}

/* ─── public entry point ─────────────────────────────────────────────────── */

int load_elf32(fs_node_t *file_node, uint32_t *entry_out) {
    Elf32_Ehdr_t ehdr;
    int rc;

    /* --- 1. Read and validate the ELF header --- */
    rc = read_exact(file_node, 0, sizeof(Elf32_Ehdr_t), (uint8_t *)&ehdr);
    if (rc != ELF_LOAD_OK) {
        kprintf("elf: could not read ELF header\n");
        return rc;
    }

    rc = validate_ehdr(&ehdr);
    if (rc != ELF_LOAD_OK) {
        return rc;
    }

    kprintf("elf: loading ELF32 executable, entry=0x%x, phnum=%d\n",
            ehdr.e_entry, ehdr.e_phnum);

    /* --- 2. Iterate program headers and load PT_LOAD segments --- */
    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        Elf32_Phdr_t phdr;
        uint32_t phdr_offset = ehdr.e_phoff + (uint32_t)(i * ehdr.e_phentsize);

        rc = read_exact(file_node, phdr_offset,
                        sizeof(Elf32_Phdr_t), (uint8_t *)&phdr);
        if (rc != ELF_LOAD_OK) {
            kprintf("elf: failed to read program header %d\n", i);
            return rc;
        }

        if (phdr.p_type != PT_LOAD) {
            continue;   /* skip non-loadable segments */
        }

        rc = load_segment(file_node, &phdr);
        if (rc != ELF_LOAD_OK) {
            return rc;
        }
    }

    /* --- 3. Return entry point to the caller --- */
    *entry_out = ehdr.e_entry;
    kprintf("elf: load complete, entry point = 0x%x\n", ehdr.e_entry);
    return ELF_LOAD_OK;
}
