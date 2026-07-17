/**
 * kernel/boot/boot_parse.c — Boot Information Structural Validation & Early Serial Logging
 *
 * Implements strict bounds checking on memory maps, modules, and framebuffer pointers.
 * Ensures the kernel never dereferences unmapped or invalid physical pointers.
 */
#include "./boot_info.h"
#include "../klibc/include/port.h"
#include "../include/uefi.h"

extern uintptr_t __end;

#define COM1_PORT 0x3F8

static void early_outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static uint8_t early_inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void early_serial_init(void) {
    early_outb(COM1_PORT + 1, 0x00); /* Disable interrupts */
    early_outb(COM1_PORT + 3, 0x80); /* Enable DLAB (set baud rate divisor) */
    early_outb(COM1_PORT + 0, 0x03); /* Divisor 3 (38400 baud) */
    early_outb(COM1_PORT + 1, 0x00);
    early_outb(COM1_PORT + 3, 0x03); /* 8 bits, no parity, one stop bit (8N1) */
    early_outb(COM1_PORT + 2, 0xC7); /* Enable FIFO, clear, 14-byte threshold */
    early_outb(COM1_PORT + 4, 0x0B); /* IRQs enabled, RTS/DSR set */
}

void early_serial_puts(const char* str) {
    if (!str) return;
    while (*str) {
        while ((early_inb(COM1_PORT + 5) & 0x20) == 0);
        early_outb(COM1_PORT, *str++);
    }
}

void early_serial_puthex(uint64_t val) {
    char buf[17];
    buf[16] = 0;
    const char hex[] = "0123456789ABCDEF";
    for (int i = 15; i >= 0; i--) {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    early_serial_puts("0x");
    early_serial_puts(buf);
}

void early_panic(const char* reason) {
    early_serial_puts("\r\n[PANIC] EARLY BOOT FAILURE: ");
    if (reason) early_serial_puts(reason);
    early_serial_puts("\r\nSystem Halted.\r\n");

    /* Also display panic message on VGA hardware buffer (0xB8000) */
    uint16_t *vga = (uint16_t*)(uintptr_t)0xB8000;
    const char prefix[] = "PANIC: ";
    int col = 0;
    for (int i = 0; prefix[i] && col < 80; i++) {
        vga[col++] = (uint16_t)prefix[i] | 0x4F00;
    }
    if (reason) {
        for (int i = 0; reason[i] && col < 80; i++) {
            vga[col++] = (uint16_t)reason[i] | 0x4F00;
        }
    }

    while (1) {
        __asm__ volatile ("cli; hlt");
    }
}

static bool is_phys_addr_safe(uint64_t paddr, uint64_t size) {
    if (paddr == 0 || size == 0) return false;
    if (paddr + size < paddr) return false; /* Overflow check */
    if (paddr < 0x1000) return false;       /* Reject null/low BIOS page traps */
    if (paddr + size > 0x10000000000ULL) return false; /* Max 1TB architectural sanity check */
    return true;
}

bool boot_info_validate_and_parse(unsigned long magic, unsigned long addr, azami_boot_info_t *out_info) {
    if (!out_info) return false;
    
    out_info->protocol = BOOT_PROTO_UNKNOWN;
    out_info->raw_magic = magic;
    out_info->raw_addr = addr;
    out_info->has_framebuffer = false;
    out_info->initrd_paddr = 0;
    out_info->initrd_size = 0;
    
    /* Calculate baseline safe free memory start after kernel end */
    uintptr_t kernel_end = (uintptr_t)&__end;
    out_info->safe_free_mem_start = (kernel_end + 4095) & ~4095UL;
    out_info->total_mem_kb = 1024 * 1024; /* Default 1GB RAM if unspecified */
    out_info->mem_lower_kb = 640;
    out_info->mem_upper_kb = 1024 * 1024 - 1024;

    /* 1. Multiboot 1 Protocol */
    if (magic == MULTIBOOT1_BOOTLOADER_MAGIC) {
        if (!is_phys_addr_safe(addr, sizeof(multiboot1_info_t))) {
            early_serial_puts("[BOOT] Multiboot 1 struct pointer out of bounds!\r\n");
            return false;
        }
        multiboot1_info_t *mb = (multiboot1_info_t*)(uintptr_t)addr;
        out_info->protocol = BOOT_PROTO_MULTIBOOT1;

        if (mb->flags & MULTIBOOT_INFO_MEMORY) {
            out_info->mem_lower_kb = mb->mem_lower;
            out_info->mem_upper_kb = mb->mem_upper;
            out_info->total_mem_kb = mb->mem_lower + mb->mem_upper + 1024;
        }

        if (mb->flags & MULTIBOOT_INFO_MODS) {
            if (mb->mods_count > 128) {
                early_serial_puts("[BOOT] Multiboot 1 modules count exceeded safety threshold!\r\n");
                return false;
            }
            if (mb->mods_count > 0) {
                if (!is_phys_addr_safe(mb->mods_addr, mb->mods_count * sizeof(multiboot1_module_t))) {
                    early_serial_puts("[BOOT] Multiboot 1 modules array pointer out of bounds!\r\n");
                    return false;
                }
                multiboot1_module_t *mods = (multiboot1_module_t*)(uintptr_t)mb->mods_addr;
                for (uint32_t m = 0; m < mb->mods_count; m++) {
                    uint64_t m_start = mods[m].mod_start;
                    uint64_t m_end = mods[m].mod_end;
                    if (m_end <= m_start || !is_phys_addr_safe(m_start, m_end - m_start)) {
                        early_serial_puts("[BOOT] Multiboot 1 module bounds check failed!\r\n");
                        return false;
                    }
                    if (m == 0) {
                        out_info->initrd_paddr = m_start;
                        out_info->initrd_size = m_end - m_start;
                    }
                    if (m_end > out_info->safe_free_mem_start && m_start < 64 * 1024 * 1024) {
                        out_info->safe_free_mem_start = (m_end + 4095) & ~4095UL;
                    }
                }
            }
        }

        if (mb->flags & MULTIBOOT_INFO_FRAMEBUFFER_INFO) {
            if (mb->framebuffer_addr != 0 && mb->framebuffer_width > 0 && mb->framebuffer_height > 0) {
                if (is_phys_addr_safe(mb->framebuffer_addr, (uint64_t)mb->framebuffer_pitch * mb->framebuffer_height)) {
                    out_info->has_framebuffer = true;
                    out_info->fb_addr = mb->framebuffer_addr;
                    out_info->fb_width = mb->framebuffer_width;
                    out_info->fb_height = mb->framebuffer_height;
                    out_info->fb_pitch = mb->framebuffer_pitch;
                    out_info->fb_bpp = mb->framebuffer_bpp;
                }
            }
        }
        return true;
    }

    /* 2. PVH Protocol (QEMU -kernel Direct 64-bit Boot) */
    if (magic == PVH_BOOTLOADER_MAGIC) {
        if (!is_phys_addr_safe(addr, sizeof(hvm_start_info_t))) {
            early_serial_puts("[BOOT] PVH hvm_start_info pointer out of bounds!\r\n");
            return false;
        }
        hvm_start_info_t *hvm = (hvm_start_info_t*)(uintptr_t)addr;
        if (hvm->magic != PVH_BOOTLOADER_MAGIC) {
            early_serial_puts("[BOOT] PVH magic mismatch inside hvm_start_info!\r\n");
            return false;
        }
        out_info->protocol = BOOT_PROTO_PVH;

        if (hvm->nr_modules > 64) {
            early_serial_puts("[BOOT] PVH modules count exceeded safety threshold!\r\n");
            return false;
        }
        if (hvm->nr_modules > 0 && hvm->modlist_paddr != 0) {
            if (!is_phys_addr_safe(hvm->modlist_paddr, hvm->nr_modules * sizeof(hvm_modlist_entry_t))) {
                early_serial_puts("[BOOT] PVH modlist pointer out of bounds!\r\n");
                return false;
            }
            hvm_modlist_entry_t *mods = (hvm_modlist_entry_t*)(uintptr_t)hvm->modlist_paddr;
            uint64_t m_start = mods[0].paddr;
            uint64_t m_size = mods[0].size;
            if (is_phys_addr_safe(m_start, m_size)) {
                out_info->initrd_paddr = m_start;
                out_info->initrd_size = m_size;
                if (m_start + m_size > out_info->safe_free_mem_start && m_start < 64 * 1024 * 1024) {
                    out_info->safe_free_mem_start = (m_start + m_size + 4095) & ~4095UL;
                }
            }
        }

        if (hvm->memmap_entries > 0 && hvm->memmap_paddr != 0) {
            if (is_phys_addr_safe(hvm->memmap_paddr, hvm->memmap_entries * sizeof(hvm_memmap_table_entry_t))) {
                hvm_memmap_table_entry_t *mmap = (hvm_memmap_table_entry_t*)(uintptr_t)hvm->memmap_paddr;
                uint64_t max_avail = 0;
                for (uint32_t i = 0; i < hvm->memmap_entries; i++) {
                    if (mmap[i].type == 1) { /* Available RAM */
                        if (mmap[i].addr + mmap[i].size > max_avail) {
                            max_avail = mmap[i].addr + mmap[i].size;
                        }
                    }
                }
                if (max_avail > 0) {
                    out_info->total_mem_kb = max_avail / 1024;
                }
            }
        }
        return true;
    }

    /* 3. Multiboot 2 Protocol */
    if (magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
        if (!is_phys_addr_safe(addr, sizeof(multiboot2_info_header_t))) {
            early_serial_puts("[BOOT] Multiboot 2 header pointer out of bounds!\r\n");
            return false;
        }
        multiboot2_info_header_t *hdr = (multiboot2_info_header_t*)(uintptr_t)addr;
        if (hdr->total_size < sizeof(multiboot2_info_header_t) || hdr->total_size > 1024 * 1024) {
            early_serial_puts("[BOOT] Multiboot 2 total size corrupted!\r\n");
            return false;
        }
        if (!is_phys_addr_safe(addr, hdr->total_size)) {
            early_serial_puts("[BOOT] Multiboot 2 tags structure exceeds safe memory boundaries!\r\n");
            return false;
        }
        out_info->protocol = BOOT_PROTO_MULTIBOOT2;

        uintptr_t tag_ptr = (uintptr_t)addr + sizeof(multiboot2_info_header_t);
        while (tag_ptr < (uintptr_t)addr + hdr->total_size) {
            multiboot2_tag_t *tag = (multiboot2_tag_t*)tag_ptr;
            if (tag->type == MULTIBOOT2_TAG_TYPE_END || tag->size == 0) break;

            if (tag->type == MULTIBOOT2_TAG_TYPE_BASIC_MEMINFO) {
                multiboot2_tag_basic_meminfo_t *mem = (multiboot2_tag_basic_meminfo_t*)tag;
                out_info->mem_lower_kb = mem->mem_lower;
                out_info->mem_upper_kb = mem->mem_upper;
                out_info->total_mem_kb = mem->mem_lower + mem->mem_upper + 1024;
            } else if (tag->type == MULTIBOOT2_TAG_TYPE_MODULE) {
                multiboot2_tag_module_t *mod = (multiboot2_tag_module_t*)tag;
                if (is_phys_addr_safe(mod->mod_start, mod->mod_end - mod->mod_start)) {
                    if (out_info->initrd_paddr == 0) {
                        out_info->initrd_paddr = mod->mod_start;
                        out_info->initrd_size = mod->mod_end - mod->mod_start;
                    }
                    if (mod->mod_end > out_info->safe_free_mem_start && mod->mod_start < 64 * 1024 * 1024) {
                        out_info->safe_free_mem_start = (mod->mod_end + 4095) & ~4095UL;
                    }
                }
            } else if (tag->type == MULTIBOOT2_TAG_TYPE_FRAMEBUFFER) {
                multiboot2_tag_framebuffer_t *fb = (multiboot2_tag_framebuffer_t*)tag;
                if (fb->framebuffer_addr != 0 && fb->framebuffer_width > 0 && fb->framebuffer_height > 0) {
                    if (is_phys_addr_safe(fb->framebuffer_addr, (uint64_t)fb->framebuffer_pitch * fb->framebuffer_height)) {
                        out_info->has_framebuffer = true;
                        out_info->fb_addr = fb->framebuffer_addr;
                        out_info->fb_width = fb->framebuffer_width;
                        out_info->fb_height = fb->framebuffer_height;
                        out_info->fb_pitch = fb->framebuffer_pitch;
                        out_info->fb_bpp = fb->framebuffer_bpp;
                    }
                }
            }
            tag_ptr += ((tag->size + 7) & ~7UL);
        }
        return true;
    }

    /* 4. UEFI Direct Boot Protocol */
    if (magic == UEFI_BOOTLOADER_MAGIC) {
        if (!is_phys_addr_safe(addr, sizeof(uefi_boot_info_t))) {
            early_serial_puts("[BOOT] UEFI boot info struct pointer out of bounds!\r\n");
            return false;
        }
        uefi_boot_info_t *ui = (uefi_boot_info_t*)(uintptr_t)addr;
        out_info->protocol = BOOT_PROTO_UEFI;
        out_info->total_mem_kb = 1024 * 1024; /* Assume 1GB for UEFI guest */

        if (ui->framebuffer_base != 0 && ui->width > 0 && ui->height > 0) {
            if (is_phys_addr_safe(ui->framebuffer_base, (uint64_t)ui->pitch * ui->height)) {
                out_info->has_framebuffer = true;
                out_info->fb_addr = ui->framebuffer_base;
                out_info->fb_width = ui->width;
                out_info->fb_height = ui->height;
                out_info->fb_pitch = ui->pitch;
                out_info->fb_bpp = ui->bpp;
            }
        }
        return true;
    }

    early_serial_puts("[BOOT] Unrecognized bootloader magic: ");
    early_serial_puthex((uint64_t)magic);
    early_serial_puts("\r\n");
    return false;
}
