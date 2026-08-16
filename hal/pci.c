/* ============================================================================
 * AzamiOS — PCI Bus Enumeration Implementation
 * File: hal/pci.c
 *
 * Uses PCI Configuration Mechanism #1 (I/O ports 0xCF8 / 0xCFC) to scan
 * all PCI buses.  Each discovered PCI function is registered as a device
 * in the kernel device tree.
 *
 * Enumeration strategy:
 *   1. Check bus 0, slot 0 to see if we have a multi-function host bridge
 *      (bit 7 of header type).  If so, scan buses 0–7 (one per function
 *      of the host bridge).  Otherwise, scan all 256 buses linearly.
 *   2. For each bus, scan all 32 slots.
 *   3. For each slot, check function 0.  If the header type's multi-
 *      function bit is set, also check functions 1–7.
 *   4. For PCI-to-PCI bridges (class 06, subclass 04), recursively scan
 *      the secondary bus.
 *
 * QEMU -M q35 exposes an Intel ICH9/Q35 chipset which has a known set of
 * PCI devices — we use this for verification.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "pci.h"
#include "../kernel/mm/kmalloc.h"
#include "../drivers/char/console.h"


/* ── Global state ────────────────────────────────────────────────────────── */
static device_t *g_pci_bus = NULL;   /* The "PCI0" device tree node */
static u32       g_pci_count = 0;    /* Total PCI functions found   */

/* ── Configuration Space Access ──────────────────────────────────────────── */

/*
 * Build the CONFIG_ADDRESS value for a given bus/slot/func/offset.
 * Bit 31:       Enable bit
 * Bits 23–16:   Bus number
 * Bits 15–11:   Device (slot) number
 * Bits 10–8:    Function number
 * Bits 7–2:     Register offset (aligned to 32 bits)
 * Bits 1–0:     Always 0 (32-bit aligned)
 */
static inline u32 pci_addr(u8 bus, u8 slot, u8 func, u8 offset)
{
    return (u32)(1U << 31)
         | ((u32)bus  << 16)
         | ((u32)(slot & 0x1F) << 11)
         | ((u32)(func & 0x07) << 8)
         | ((u32)(offset & 0xFC));
}

u32 pci_config_read32(u8 bus, u8 slot, u8 func, u8 offset)
{
    outl(PCI_CONFIG_ADDR, pci_addr(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

u16 pci_config_read16(u8 bus, u8 slot, u8 func, u8 offset)
{
    outl(PCI_CONFIG_ADDR, pci_addr(bus, slot, func, offset));
    return (u16)(inl(PCI_CONFIG_DATA) >> ((offset & 2) * 8));
}

u8 pci_config_read8(u8 bus, u8 slot, u8 func, u8 offset)
{
    outl(PCI_CONFIG_ADDR, pci_addr(bus, slot, func, offset));
    return (u8)(inl(PCI_CONFIG_DATA) >> ((offset & 3) * 8));
}

void pci_config_write32(u8 bus, u8 slot, u8 func, u8 offset, u32 val)
{
    outl(PCI_CONFIG_ADDR, pci_addr(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, val);
}

void pci_config_write16(u8 bus, u8 slot, u8 func, u8 offset, u16 val)
{
    outl(PCI_CONFIG_ADDR, pci_addr(bus, slot, func, offset));
    outw(PCI_CONFIG_DATA + (offset & 2), val);
}

void pci_config_write8(u8 bus, u8 slot, u8 func, u8 offset, u8 val)
{
    outl(PCI_CONFIG_ADDR, pci_addr(bus, slot, func, offset));
    outb(PCI_CONFIG_DATA + (offset & 3), val);
}

/* ── PCI Class Name Lookup ───────────────────────────────────────────────── */

static const char *pci_class_name(u8 class_code, u8 subclass)
{
    switch (class_code) {
    case PCI_CLASS_UNCLASSIFIED:
        return subclass == 0x01 ? "VGA-Compatible" : "Unclassified";
    case PCI_CLASS_MASS_STORAGE:
        switch (subclass) {
        case 0x01: return "IDE Controller";
        case 0x05: return "ATA Controller";
        case 0x06: return "SATA Controller";
        case 0x08: return "NVMe Controller";
        default:   return "Mass Storage";
        }
    case PCI_CLASS_NETWORK:
        return subclass == 0x00 ? "Ethernet Controller" : "Network";
    case PCI_CLASS_DISPLAY:
        return subclass == 0x00 ? "VGA Controller" : "Display";
    case PCI_CLASS_MULTIMEDIA:
        switch (subclass) {
        case 0x01: return "Audio Controller";
        case 0x03: return "HD Audio";
        default:   return "Multimedia";
        }
    case PCI_CLASS_MEMORY:
        return "Memory Controller";
    case PCI_CLASS_BRIDGE:
        switch (subclass) {
        case 0x00: return "Host Bridge";
        case 0x01: return "ISA Bridge";
        case 0x04: return "PCI-to-PCI Bridge";
        case 0x80: return "Other Bridge";
        default:   return "Bridge";
        }
    case PCI_CLASS_COMMUNICATION:
        return "Communication";
    case PCI_CLASS_SYSTEM:
        switch (subclass) {
        case 0x00: return "PIC";
        case 0x01: return "DMA Controller";
        case 0x02: return "Timer";
        case 0x03: return "RTC";
        case 0x05: return "SD Host Controller";
        case 0x80: return "System Peripheral";
        default:   return "System";
        }
    case PCI_CLASS_INPUT:
        return "Input Device";
    case PCI_CLASS_SERIAL_BUS:
        switch (subclass) {
        case 0x03: return "USB Controller";
        case 0x05: return "SMBus Controller";
        default:   return "Serial Bus";
        }
    default:
        return "Unknown";
    }
}

/* Map PCI class code to our device type enum */
static device_type_t pci_class_to_device_type(u8 class_code)
{
    switch (class_code) {
    case PCI_CLASS_MASS_STORAGE:  return DEVICE_TYPE_BLOCK;
    case PCI_CLASS_NETWORK:      return DEVICE_TYPE_NETWORK;
    case PCI_CLASS_DISPLAY:      return DEVICE_TYPE_DISPLAY;
    case PCI_CLASS_MULTIMEDIA:   return DEVICE_TYPE_AUDIO;
    case PCI_CLASS_INPUT:        return DEVICE_TYPE_INPUT;
    case PCI_CLASS_BRIDGE:       return DEVICE_TYPE_BUS;
    default:                     return DEVICE_TYPE_OTHER;
    }
}

/* ── Build a device name from the bus/slot/func ──────────────────────────── */

static void pci_device_name(char *buf, size_t len, u8 bus, u8 slot, u8 func)
{
    /* Format: "PCI_BB:SS.F" */
    const char hex[] = "0123456789ABCDEF";
    size_t i = 0;

    if (len < 14) { buf[0] = '\0'; return; }

    buf[i++] = 'P';
    buf[i++] = 'C';
    buf[i++] = 'I';
    buf[i++] = '_';
    buf[i++] = hex[(bus >> 4) & 0xF];
    buf[i++] = hex[bus & 0xF];
    buf[i++] = ':';
    buf[i++] = hex[(slot >> 4) & 0xF];
    buf[i++] = hex[slot & 0xF];
    buf[i++] = '.';
    buf[i++] = hex[func & 0xF];
    buf[i]   = '\0';
}

/* ── Scan a single PCI function ──────────────────────────────────────────── */

static void pci_scan_function(u8 bus, u8 slot, u8 func);
static void pci_scan_bus(u8 bus);

static void pci_scan_function(u8 bus, u8 slot, u8 func)
{
    u16 vendor = pci_config_read16(bus, slot, func, PCI_VENDOR_ID);
    if (vendor == 0xFFFF) return;   /* No device at this function */

    u16 device_id   = pci_config_read16(bus, slot, func, PCI_DEVICE_ID);
    u8  class_code  = pci_config_read8(bus, slot, func, PCI_CLASS_CODE);
    u8  subclass    = pci_config_read8(bus, slot, func, PCI_SUBCLASS);
    u8  prog_if     = pci_config_read8(bus, slot, func, PCI_PROG_IF);
    u8  revision    = pci_config_read8(bus, slot, func, PCI_REVISION_ID);
    u8  header_type = pci_config_read8(bus, slot, func, PCI_HEADER_TYPE);
    u8  irq_line    = pci_config_read8(bus, slot, func, PCI_INTERRUPT_LINE);
    u8  irq_pin     = pci_config_read8(bus, slot, func, PCI_INTERRUPT_PIN);

    /* Build device info structure */
    pci_device_info_t *info = (pci_device_info_t *)kzalloc(sizeof(pci_device_info_t));
    if (!info) return;

    info->vendor_id   = vendor;
    info->device_id   = device_id;
    info->class_code  = class_code;
    info->subclass    = subclass;
    info->prog_if     = prog_if;
    info->revision    = revision;
    info->bus         = bus;
    info->slot        = slot;
    info->func        = func;
    info->header_type = header_type & 0x7F;  /* Mask off multi-function bit */
    info->interrupt_line = irq_line;
    info->interrupt_pin  = irq_pin;

    /* Read BARs (only for standard header type 0) */
    if (info->header_type == 0x00) {
        for (int i = 0; i < 6; i++) {
            info->bar[i] = pci_config_read32(bus, slot, func,
                                              PCI_BAR0 + (u8)(i * 4));
        }
        info->subsys_vendor = pci_config_read16(bus, slot, func, PCI_SUBSYS_VENDOR);
        info->subsys_id     = pci_config_read16(bus, slot, func, PCI_SUBSYS_ID);
    }

    /* Create a device tree node */
    char name[32];
    pci_device_name(name, sizeof(name), bus, slot, func);

    device_type_t dev_type = pci_class_to_device_type(class_code);
    device_t *dev = device_create(name, dev_type, g_pci_bus);
    if (!dev) {
        kfree(info);
        return;
    }

    dev->driver_data = info;
    g_pci_count++;

    const char *desc = pci_class_name(class_code, subclass);
    pr_debug("[PCI] %02x:%02x.%x  %04x:%04x  %02x.%02x  %s",
            bus, slot, func,
            vendor, device_id,
            class_code, subclass,
            desc);
    if (irq_pin) {
        kprintf("  IRQ=%u", irq_line);
    }
    kprintf("\n");

    /* If this is a PCI-to-PCI bridge, recursively scan the secondary bus. */
    if (class_code == PCI_CLASS_BRIDGE && subclass == 0x04) {
        u8 secondary_bus = pci_config_read8(bus, slot, func, PCI_SECONDARY_BUS);
        if (secondary_bus != 0) {
            pci_scan_bus(secondary_bus);
        }
    }
}

/* ── Scan a single slot (up to 8 functions) ──────────────────────────────── */

static void pci_scan_slot(u8 bus, u8 slot)
{
    u16 vendor = pci_config_read16(bus, slot, 0, PCI_VENDOR_ID);
    if (vendor == 0xFFFF) return;   /* Empty slot */

    pci_scan_function(bus, slot, 0);

    /* If function 0 is multi-function, also check functions 1–7. */
    u8 header_type = pci_config_read8(bus, slot, 0, PCI_HEADER_TYPE);
    if (header_type & 0x80) {
        for (u8 func = 1; func < 8; func++) {
            pci_scan_function(bus, slot, func);
        }
    }
}

static u8 g_scanned_buses[32] = {0};

static void pci_scan_bus(u8 bus)
{
    if (g_scanned_buses[bus / 8] & (1 << (bus % 8))) return;
    g_scanned_buses[bus / 8] |= (1 << (bus % 8));

    for (u8 slot = 0; slot < 32; slot++) {
        pci_scan_slot(bus, slot);
    }
}

/* ── Device Helpers ──────────────────────────────────────────────────────── */

void pci_enable_bus_mastering(device_t *dev)
{
    pci_device_info_t *info = pci_get_device_info(dev);
    if (!info) return;

    u16 cmd = pci_config_read16(info->bus, info->slot, info->func, PCI_COMMAND);
    cmd |= PCI_CMD_BUS_MASTER | PCI_CMD_MEMORY_SPACE | PCI_CMD_IO_SPACE;
    pci_config_write16(info->bus, info->slot, info->func, PCI_COMMAND, cmd);
}

phys_addr_t pci_get_bar(device_t *dev, u32 bar_index)
{
    pci_device_info_t *info = pci_get_device_info(dev);
    if (!info || bar_index >= 6) return 0;

    u32 bar = info->bar[bar_index];

    if (bar & 0x01) {
        /* I/O space BAR — mask off bit 0 (type) and bit 1 (reserved) */
        return (phys_addr_t)(bar & ~0x3U);
    } else {
        /* Memory space BAR — mask off bits 0-3 (type, prefetchable) */
        phys_addr_t addr = (phys_addr_t)(bar & ~0xFU);

        /* Check for 64-bit BAR (type bits 1:2 == 0b10) */
        if (((bar >> 1) & 0x3) == 0x02 && bar_index < 5) {
            u32 bar_high = info->bar[bar_index + 1];
            addr |= ((phys_addr_t)bar_high << 32);
        }

        return addr;
    }
}

pci_device_info_t *pci_get_device_info(device_t *dev)
{
    if (!dev) return NULL;
    return (pci_device_info_t *)dev->driver_data;
}

u32 pci_get_device_count(void)
{
    return g_pci_count;
}

/* ── PCI Initialization ──────────────────────────────────────────────────── */

void pci_init(void)
{
    /* Create the PCI bus root device */
    g_pci_bus = device_create("PCI0", DEVICE_TYPE_BUS, NULL);
    if (!g_pci_bus) {
        pr_debug("[PCI] ERROR: Could not create PCI bus device\n");
        return;
    }
    g_pci_bus->flags |= DEVICE_FLAG_BUS_DRIVER;

    pr_debug("[PCI] Scanning PCI buses...\n");

    /* Check if the host bridge (bus 0, slot 0) is multi-function.
     * If yes, each function represents a separate PCI bus. */
    u8 host_header = pci_config_read8(0, 0, 0, PCI_HEADER_TYPE);
    if (host_header & 0x80) {
        /* Multi-function host bridge: scan bus per function */
        for (u8 func = 0; func < 8; func++) {
            u16 vendor = pci_config_read16(0, 0, func, PCI_VENDOR_ID);
            if (vendor != 0xFFFF) {
                pci_scan_bus(func);
            }
        }
    } else {
        /* Single-function host bridge: scan all 256 buses */
        for (u16 bus = 0; bus < 256; bus++) {
            pci_scan_bus((u8)bus);
        }
    }

    pr_debug("[PCI] Enumeration complete: %u device(s) found\n", g_pci_count);
}
