/* ============================================================================
 * AzamiOS — IO APIC Driver
 * File: drivers/ioapic.c
 * ============================================================================ */

#include "ioapic.h"
#include "acpi.h"
#include "../../include/azami/defs.h"
#include <azami/debug.h>
#include "../../arch/x86_64/boot/limine_req.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../kernel/panic.h"

#define MAX_IOAPICS 8
#define MAX_ISO 16

typedef struct {
    volatile u32 *base;
    u32 gsi_base;
    u32 max_intr;
} ioapic_entry_t;

static ioapic_entry_t g_ioapics[MAX_IOAPICS];
static u32 g_ioapic_count = 0;

typedef struct {
    u32 gsi;
    u16 flags;
} iso_entry_t;

static iso_entry_t g_iso_map[MAX_ISO];

static inline void *phys_to_virt(u64 phys)
{
    return (void *)(phys + az_boot_hhdm_base());
}

static u32 ioapic_read(volatile u32 *base, u32 reg)
{
    base[0] = reg;
    return base[4]; /* IOWIN is at offset 0x10 */
}

static void ioapic_write(volatile u32 *base, u32 reg, u32 val)
{
    base[0] = reg;
    base[4] = val;
}

static ioapic_entry_t *ioapic_for_gsi(u32 gsi)
{
    for (u32 i = 0; i < g_ioapic_count; i++) {
        if (gsi >= g_ioapics[i].gsi_base && gsi < g_ioapics[i].gsi_base + g_ioapics[i].max_intr) {
            return &g_ioapics[i];
        }
    }
    return NULL;
}

void ioapic_init(void)
{
    /* Initialize ISO map 1:1 by default (edge triggered, active high) */
    for (int i = 0; i < MAX_ISO; i++) {
        g_iso_map[i].gsi = i;
        g_iso_map[i].flags = 0;
    }

    acpi_madt_t *madt = (acpi_madt_t *)acpi_find_table("APIC");
    if (!madt) {
        pr_debug("[IOAPIC] Error: MADT not found.\n");
        return;
    }

    u8 *ptr = (u8 *)madt + sizeof(acpi_madt_t);
    u8 *end = (u8 *)madt + madt->header.length;

    while (ptr < end) {
        acpi_madt_record_t *record = (acpi_madt_record_t *)ptr;
        
        if (record->type == ACPI_MADT_TYPE_IOAPIC) {
            if (g_ioapic_count >= MAX_IOAPICS) {
                pr_debug("[IOAPIC] Warning: Max IO APICs reached.\n");
            } else {
                acpi_madt_ioapic_t *ioapic = (acpi_madt_ioapic_t *)record;
                ioapic_entry_t *entry = &g_ioapics[g_ioapic_count++];
                
                entry->gsi_base = ioapic->gsi_base;
                entry->base = (volatile u32 *)phys_to_virt(ioapic->ioapic_addr);
                
                /* Map IO APIC as MMIO */
                vmm_map(vmm_kernel_space(), (virt_addr_t)entry->base, 
                        (phys_addr_t)ioapic->ioapic_addr, VMM_MMIO);
                        
                /* Get Max Redirection Entries */
                u32 ver = ioapic_read(entry->base, IOAPIC_REG_VER);
                entry->max_intr = ((ver >> 16) & 0xFF) + 1;
                
                pr_debug("[IOAPIC] Found IO APIC at 0x%08x, GSI Base %u, Max Intrs: %u\n",
                         ioapic->ioapic_addr, entry->gsi_base, entry->max_intr);
            }
        }
        else if (record->type == ACPI_MADT_TYPE_ISO) {
            acpi_madt_iso_t *iso = (acpi_madt_iso_t *)record;
            if (iso->source_irq < MAX_ISO) {
                g_iso_map[iso->source_irq].gsi = iso->global_system_interrupt;
                g_iso_map[iso->source_irq].flags = iso->flags;
                pr_debug("[IOAPIC] Override: IRQ %u -> GSI %u (flags: 0x%x)\n", 
                         iso->source_irq, iso->global_system_interrupt, iso->flags);
            }
        }
        
        ptr += record->length;
    }
}

void ioapic_set_irq(u8 irq, u8 vector, u32 lapic_id)
{
    if (g_ioapic_count == 0) return;

    u32 gsi = (irq < MAX_ISO) ? g_iso_map[irq].gsi : irq;
    u16 flags = (irq < MAX_ISO) ? g_iso_map[irq].flags : 0;
    
    ioapic_entry_t *ioapic = ioapic_for_gsi(gsi);
    if (!ioapic) return;

    u32 reg = IOAPIC_REG_REDTBL0 + (gsi - ioapic->gsi_base) * 2;
    
    u32 low = vector & 0xFF;
    
    /* Active low? (Flags bit 1: 01=High, 11=Low) */
    if ((flags & 3) == 3) low |= (1 << 13);
    
    /* Level triggered? (Flags bit 3: 01=Edge, 11=Level) */
    if (((flags >> 2) & 3) == 3) low |= (1 << 15);
    
    /* Unmasked */
    low &= ~(1 << 16);
    
    /* Write high register first while masked */
    u32 high = (lapic_id & 0xFF) << 24;
    ioapic_write(ioapic->base, reg, low | (1 << 16));
    ioapic_write(ioapic->base, reg + 1, high);
    ioapic_write(ioapic->base, reg, low);
}

void ioapic_mask_irq(u8 irq)
{
    if (g_ioapic_count == 0) return;

    u32 gsi = (irq < MAX_ISO) ? g_iso_map[irq].gsi : irq;
    
    ioapic_entry_t *ioapic = ioapic_for_gsi(gsi);
    if (!ioapic) return;

    u32 reg = IOAPIC_REG_REDTBL0 + (gsi - ioapic->gsi_base) * 2;
    
    u32 low = ioapic_read(ioapic->base, reg);
    ioapic_write(ioapic->base, reg, low | (1 << 16));
}

bool ioapic_is_active(void)
{
    return g_ioapic_count > 0;
}
