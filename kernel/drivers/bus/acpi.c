/**
 * acpi.c  –  Advanced Configuration and Power Interface Driver
 * Table discovery, power management parsing, shutdown & reboot
 */
#include "./include/acpi.h"
#include "../klibc/include/stdio.h"
#include "../klibc/include/string.h"
#include "../klibc/include/port.h"
#include "../mem/include/paging.h"
#include <stddef.h>

static acpi_rsdp_t *g_rsdp = NULL;
static acpi_rsdt_t *g_rsdt = NULL;
static acpi_fadt_t *g_fadt = NULL;
static acpi_madt_t *g_madt = NULL;

static uint32_t g_pm1a_cnt_blk = 0;
static uint32_t g_pm1b_cnt_blk = 0;
static uint16_t g_slp_typa = 0;
static uint16_t g_slp_typb = 0;
static bool     g_acpi_enabled = false;

/* Checksum validation for standard ACPI SDT headers */
static bool acpi_validate_checksum(acpi_sdt_hdr_t *hdr) {
    uint8_t sum = 0;
    uint8_t *b = (uint8_t*)hdr;
    for (uint32_t i = 0; i < hdr->length; i++) {
        sum += b[i];
    }
    return sum == 0;
}

static void acpi_map_table(uint32_t addr) {
    if (!addr) return;
    paging_map_page(addr & ~0xFFFu, addr & ~0xFFFu, 1, 0);
    paging_map_page((addr & ~0xFFFu) + 4096, (addr & ~0xFFFu) + 4096, 1, 0);
    acpi_sdt_hdr_t *hdr = (acpi_sdt_hdr_t*)addr;
    uint32_t len = hdr->length;
    if (len > 1024 * 1024) len = 4096;
    uint32_t start_page = addr & ~0xFFFu;
    uint32_t end_page = (addr + len + 0xFFFu) & ~0xFFFu;
    for (uint32_t p = start_page; p < end_page; p += 4096) {
        paging_map_page(p, p, 1, 0);
    }
}

/* Parse DSDT table bytecode for _S5_ sleep package */
static void acpi_parse_dsdt(uint32_t dsdt_addr) {
    if (!dsdt_addr) return;
    acpi_map_table(dsdt_addr);
    acpi_sdt_hdr_t *hdr = (acpi_sdt_hdr_t*)dsdt_addr;
    if (strncmp(hdr->signature, "DSDT", 4) != 0) return;

    uint8_t *p = (uint8_t*)dsdt_addr + sizeof(acpi_sdt_hdr_t);
    uint32_t len = hdr->length - sizeof(acpi_sdt_hdr_t);

    for (uint32_t i = 0; i < len - 7; i++) {
        if (p[i] == '_' && p[i+1] == 'S' && p[i+2] == '5' && p[i+3] == '_') {
            uint32_t j = i + 4;
            if (p[j] == 0x12 || (p[j] == 0x08 && p[j+1] == 0x12)) {
                if (p[j] == 0x08) j++;
                j += 2; /* skip 0x12 package op and pkglen */
                if (p[j] == 0x0A) j++; /* byte prefix */
                g_slp_typa = p[j] << 10;
                j++;
                if (p[j] == 0x0A) j++;
                g_slp_typb = p[j] << 10;
                return;
            }
        }
    }
    /* Fallback standard S5 values (QEMU / Bochs / Real PCs) */
    g_slp_typa = (5 << 10); /* 0x1400 */
    g_slp_typb = (5 << 10);
}

void acpi_init(void) {
    kprintf("acpi: probing system memory for RSDP...\n");

    /* 1. Search EBDA (0x9FC00..0x9FFFF) and BIOS ROM space (0xE0000..0xFFFFF) */
    uint32_t search_areas[2][2] = {
        { 0x9FC00, 0xA0000 },
        { 0xE0000, 0x100000 }
    };

    for (int area = 0; area < 2 && !g_rsdp; area++) {
        for (uint32_t addr = search_areas[area][0]; addr < search_areas[area][1]; addr += 16) {
            if (strncmp((char*)addr, "RSD PTR ", 8) == 0) {
                acpi_rsdp_t *r = (acpi_rsdp_t*)addr;
                uint8_t sum = 0;
                uint8_t *b = (uint8_t*)r;
                for (int i = 0; i < 20; i++) sum += b[i];
                if (sum == 0) {
                    g_rsdp = r;
                    break;
                }
            }
        }
    }

    if (!g_rsdp) {
        kprintf("acpi: RSDP signature not found in memory\n");
        return;
    }

    char oem[7];
    memcpy(oem, g_rsdp->oem_id, 6);
    oem[6] = 0;
    kprintf("acpi: found RSDP at 0x%x (OEM: %s, rev %d)\n", g_rsdp, oem, g_rsdp->revision);

    /* 2. Parse RSDT */
    acpi_map_table(g_rsdp->rsdt_address);
    g_rsdt = (acpi_rsdt_t*)g_rsdp->rsdt_address;
    if (!acpi_validate_checksum(&g_rsdt->header)) {
        kprintf("acpi: RSDT checksum invalid\n");
        return;
    }

    uint32_t num_tables = (g_rsdt->header.length - sizeof(acpi_sdt_hdr_t)) / 4;
    kprintf("acpi: RSDT at 0x%x contains %d tables\n", g_rsdt, num_tables);

    for (uint32_t i = 0; i < num_tables; i++) {
        acpi_map_table(g_rsdt->table_pointers[i]);
        acpi_sdt_hdr_t *hdr = (acpi_sdt_hdr_t*)g_rsdt->table_pointers[i];
        char sig[5];
        memcpy(sig, hdr->signature, 4);
        sig[4] = 0;

        if (strcmp(sig, "FACP") == 0) {
            g_fadt = (acpi_fadt_t*)hdr;
            if (acpi_validate_checksum(hdr)) {
                g_pm1a_cnt_blk = g_fadt->pm1a_cnt_blk;
                g_pm1b_cnt_blk = g_fadt->pm1b_cnt_blk;
                acpi_parse_dsdt(g_fadt->dsdt);
                kprintf("acpi: FADT found (PM1a_CNT=0x%x, DSDT=0x%x)\n", g_pm1a_cnt_blk, g_fadt->dsdt);
            }
        } else if (strcmp(sig, "APIC") == 0) {
            g_madt = (acpi_madt_t*)hdr;
            if (acpi_validate_checksum(hdr)) {
                kprintf("acpi: MADT found (Local APIC base=0x%x)\n", g_madt->lapic_address);
            }
        }
    }

    g_acpi_enabled = true;
}

void acpi_poweroff(void) {
    kprintf("\nacpi: executing hardware shutdown sequence...\n");

    /* Write sleep mode to ACPI PM1a control block (bit 13 = SLP_EN) */
    if (g_pm1a_cnt_blk) {
        outw(g_pm1a_cnt_blk, g_slp_typa | 0x2000);
        if (g_pm1b_cnt_blk) {
            outw(g_pm1b_cnt_blk, g_slp_typb | 0x2000);
        }
    }

    /* Hypervisor fallback ports */
    outw(0x604, 0x2000);   /* QEMU / Bochs older ACPI poweroff port */
    outw(0x4004, 0x3400);  /* VirtualBox poweroff port */
    outw(0xB004, 0x2000);  /* Bochs poweroff */

    asm volatile("cli");
    for (;;) asm volatile("hlt");
}

void acpi_reboot(void) {
    kprintf("\nacpi: executing hardware reboot...\n");

    /* 8042 Keyboard Controller Reset Pulse */
    uint8_t temp;
    do {
        temp = inb(0x64);
        if (temp & 1) inb(0x60);
    } while (temp & 2);
    outb(0x64, 0xFE);

    asm volatile("cli");
    for (;;) asm volatile("hlt");
}

uint32_t acpi_get_lapic_base(void) {
    return g_madt ? g_madt->lapic_address : 0xFEE00000;
}

bool acpi_is_enabled(void) {
    return g_acpi_enabled;
}

void acpi_print_info(void) {
    if (!g_acpi_enabled) {
        kprintf("ACPI: Not initialized or disabled\n");
        return;
    }
    char oem[7];
    memcpy(oem, g_rsdp->oem_id, 6);
    oem[6] = 0;
    kprintf("ACPI Root Pointer: 0x%x (OEM: %s, v%d)\n", g_rsdp, oem, g_rsdp->revision);
    kprintf("RSDT Table Address: 0x%x\n", g_rsdt);
    if (g_fadt) {
        kprintf("FADT PM1a_CNT Port: 0x%x (SLP_TYPa: 0x%x)\n", g_pm1a_cnt_blk, g_slp_typa);
    }
    if (g_madt) {
        kprintf("MADT Local APIC Base: 0x%x\n", g_madt->lapic_address);
    }
}
