/* ============================================================================
 * AzamiOS — ACPI Subsystem
 * File: drivers/acpi.c
 * ============================================================================ */

#include "acpi.h"
#define DEBUG 1
#include <azami/debug.h>
#include "../../include/azami/defs.h"
#include "../../arch/x86_64/boot/limine_req.h"
#include "../../kernel/lib/string.h"
#include "../../kernel/panic.h"
#include "../../arch/x86_64/mm/vmm.h"

#define MAX_ACPI_TABLES 64

static acpi_sdt_header_t *g_acpi_tables[MAX_ACPI_TABLES];
static u32 g_acpi_table_count = 0;

static acpi_fadt_t *g_fadt = NULL;
static u16 g_slp_typa = 0;
static u16 g_slp_typb = 0;

/* Helper to convert physical addresses found in ACPI tables to virtual addresses */
static inline void *phys_to_virt(u64 phys)
{
    return (void *)(phys + az_boot_hhdm_base());
}

static bool acpi_checksum(void *table, u32 length)
{
    u8 sum = 0;
    u8 *ptr = (u8 *)table;
    for (u32 i = 0; i < length; i++) {
        sum += ptr[i];
    }
    return sum == 0;
}

void *acpi_find_table(const char *signature)
{
    for (u32 i = 0; i < g_acpi_table_count; i++) {
        if (memcmp(g_acpi_tables[i]->signature, signature, 4) == 0) {
            return g_acpi_tables[i];
        }
    }
    return NULL;
}

/* Rudimentary AML byte-scanner to find \_S5_ and extract SLP_TYP values */
static void parse_s5(u8 *aml, u32 length)
{
    for (u32 i = 0; i < length - 7; i++) {
        /* Search for '_S5_' */
        if (aml[i] == '_' && aml[i+1] == 'S' && aml[i+2] == '5' && aml[i+3] == '_') {
            
            /* Verify it's part of a NameOp (0x08) optionally prefixed by RootChar (\) */
            bool is_valid = false;
            if (i >= 1 && aml[i-1] == 0x08) is_valid = true;
            if (i >= 2 && aml[i-2] == 0x08 && aml[i-1] == '\\') is_valid = true;
            
            if (is_valid && aml[i+4] == 0x12) { /* PackageOp follows */
                u8 *pkg = &aml[i+5];
                u8 num_elements;
                u8 *elements;
                
                /* Package length encoding varies */
                if ((pkg[0] & 0xC0) == 0) {
                    num_elements = pkg[1];
                    elements = &pkg[2];
                } else {
                    /* Multi-byte length (assume 2 bytes for simplicity) */
                    num_elements = pkg[2];
                    elements = &pkg[3];
                }
                
                if (num_elements >= 2) {
                    /* First element: SLP_TYPa */
                    if (elements[0] == 0x0A) g_slp_typa = elements[1];
                    else g_slp_typa = elements[0];
                    
                    /* Second element: SLP_TYPb */
                    u8 *elem2 = elements;
                    if (elem2[0] == 0x0A) elem2 += 2;
                    else elem2 += 1;
                    
                    if (elem2[0] == 0x0A) g_slp_typb = elem2[1];
                    else g_slp_typb = elem2[0];
                    
                    pr_debug("[ACPI] Found \\_S5_ package: SLP_TYPa=%u, SLP_TYPb=%u\n", g_slp_typa, g_slp_typb);
                }
                return;
            }
        }
    }
    pr_debug("[ACPI] \\_S5_ package not found in DSDT.\n");
}

void acpi_init(void)
{
    acpi_rsdp_t *rsdp = (acpi_rsdp_t *)az_boot_rsdp();
    if (!rsdp) {
        pr_debug("[ACPI] Error: Limine did not provide an RSDP.\n");
        return;
    }
    
    if ((u64)rsdp < az_boot_hhdm_base()) {
        rsdp = (acpi_rsdp_t *)phys_to_virt((u64)rsdp);
    }
    
    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0 || !acpi_checksum(rsdp, 20)) {
        pr_debug("[ACPI] Error: Invalid RSDP signature or checksum.\n");
        return;
    }

    pr_debug("[ACPI] Found RSDP v%u. OEM: %.6s\n", rsdp->revision, rsdp->oem_id);

    acpi_sdt_header_t *root_sdt = NULL;
    u32 entries = 0;
    bool is_xsdt = false;

    if (rsdp->revision >= 2 && rsdp->xsdt_address != 0) {
        root_sdt = (acpi_sdt_header_t *)phys_to_virt(rsdp->xsdt_address);
        is_xsdt = true;
    } else {
        root_sdt = (acpi_sdt_header_t *)phys_to_virt(rsdp->rsdt_address);
    }

    if (!acpi_checksum(root_sdt, root_sdt->length)) {
        pr_debug("[ACPI] Error: Invalid %sDT checksum.\n", is_xsdt ? "XS" : "RS");
        return;
    }

    entries = (root_sdt->length - sizeof(acpi_sdt_header_t)) / (is_xsdt ? 8 : 4);
    
    /* Parse the root table */
    for (u32 i = 0; i < entries && g_acpi_table_count < MAX_ACPI_TABLES; i++) {
        acpi_sdt_header_t *sdt;
        if (is_xsdt) {
            u64 phys = *((u64 *)((u8 *)root_sdt + sizeof(acpi_sdt_header_t) + (i * 8)));
            sdt = (acpi_sdt_header_t *)phys_to_virt(phys);
        } else {
            u32 phys = *((u32 *)((u8 *)root_sdt + sizeof(acpi_sdt_header_t) + (i * 4)));
            sdt = (acpi_sdt_header_t *)phys_to_virt(phys);
        }
        
        if (acpi_checksum(sdt, sdt->length)) {
            g_acpi_tables[g_acpi_table_count++] = sdt;
            pr_debug("[ACPI] Found table: %.4s\n", sdt->signature);
        }
    }

    /* Process FADT for power management */
    g_fadt = (acpi_fadt_t *)acpi_find_table("FACP");
    if (g_fadt) {
        /* Enable ACPI mode if not already enabled */
        if (g_fadt->smi_cmd && g_fadt->acpi_enable) {
            outb(g_fadt->smi_cmd, g_fadt->acpi_enable);
            /* Give it time to transition */
            for (volatile int i = 0; i < 100000; i++) cpu_pause();
            pr_debug("[ACPI] Transitioned to ACPI mode.\n");
        }
        
        /* Find DSDT and parse for \_S5_ */
        u64 dsdt_phys = (g_fadt->header.revision >= 2 && g_fadt->x_dsdt) ? g_fadt->x_dsdt : g_fadt->dsdt;
        if (dsdt_phys) {
            acpi_sdt_header_t *dsdt = (acpi_sdt_header_t *)phys_to_virt(dsdt_phys);
            if (acpi_checksum(dsdt, dsdt->length)) {
                parse_s5((u8 *)dsdt + sizeof(acpi_sdt_header_t), dsdt->length - sizeof(acpi_sdt_header_t));
            }
        }
    }
}

void acpi_reboot(void)
{
    if (!g_fadt) return;
    
    /* ACPI 2.0+ Reboot mechanism (RESET_REG_SUP = bit 10 of flags) */
    if (g_fadt->flags & (1 << 10)) {
        u8 reset_val = g_fadt->reset_value;
        if (g_fadt->reset_reg.address_space == 1) { /* System I/O */
            outb(g_fadt->reset_reg.address, reset_val);
        } else if (g_fadt->reset_reg.address_space == 0) { /* System Memory */
            volatile u8 *mem = (volatile u8 *)phys_to_virt(g_fadt->reset_reg.address);
            *mem = reset_val;
        }
    }
    
    /* Fallback: 8042 keyboard controller reset */
    u8 good = 0x02;
    while (good & 0x02) {
        good = inb(0x64);
    }
    outb(0x64, 0xFE);
    
    /* Halt if all fails */
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

void acpi_shutdown(void)
{
    if (!g_fadt || !g_fadt->pm1a_cnt_blk) return;
    
    /* ACPI Soft-Off uses SLP_EN bit (13) + SLP_TYP */
    u16 slp_en = 1 << 13;
    
    outw(g_fadt->pm1a_cnt_blk, g_slp_typa | slp_en);
    
    if (g_fadt->pm1b_cnt_blk) {
        outw(g_fadt->pm1b_cnt_blk, g_slp_typb | slp_en);
    }
    
    /* Fallback for QEMU/Bochs/VirtualBox if ACPI fails or \_S5_ not found */
    outw(0xB004, 0x2000); /* Bochs/QEMU */
    outw(0x604, 0x2000);  /* Older QEMU */
    outw(0x4004, 0x3400); /* VirtualBox */
    
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
