/* ============================================================================
 * AzamiOS — Intel AC97 Audio Driver Implementation
 * File: drivers/sound/ac97.c
 * ============================================================================ */

#define DEBUG 1
#define DEBUG 1
#include "../../include/azami/debug.h"
#include "ac97.h"
#include "sound.h"
#include "../../hal/pci.h"
#include "../../hal/device.h"
#include "../../arch/x86_64/cpu/pic.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../kernel/mm/pmm.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../arch/x86_64/cpu/idt.h"
#include "../../hal/irq.h"
#include "../../kernel/uaccess.h"

extern void idt_register_irq(u8 vector, void (*handler)(pt_regs_t *, void *), void *ctx);
extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);

static u32 g_nam_bar = 0;
static u32 g_nabm_bar = 0;
static u8  g_ac97_irq = 0;

#define AC97_BDL_ENTRIES 32
#define AC97_BUFFER_SIZE 4096

static ac97_bdl_entry_t *g_bdl;
static u8 *g_audio_buffers[AC97_BDL_ENTRIES];
static spinlock_t g_ac97_lock = SPINLOCK_INIT;
static u8 g_ac97_lvi = 0;
static bool g_ac97_started = false;

static sound_device_t g_ac97_sound_dev;

static void ac97_outb(u32 bar, u16 offset, u8 val)   { outb(bar + offset, val); }
static void ac97_outw(u32 bar, u16 offset, u16 val)  { outw(bar + offset, val); }
static void ac97_outd(u32 bar, u16 offset, u32 val)  { outl(bar + offset, val); }

static u8  ac97_inb(u32 bar, u16 offset)   { return inb(bar + offset); }
static u16 ac97_inw(u32 bar, u16 offset)   { return inw(bar + offset); }

static void ac97_irq_handler(pt_regs_t *r, void *ctx)
{
    (void)r; (void)ctx;
    u16 status = ac97_inw(g_nabm_bar, AC97_PO_SR);
    if (status & 0x1C) {
        ac97_outw(g_nabm_bar, AC97_PO_SR, status & 0x1C);
    }
}

static s64 ac97_write_pcm(const u8 *data, u64 len)
{
    irqflags_t irqf = spinlock_lock_irqsave(&g_ac97_lock);

    u64 written = 0;
    u8 cr = ac97_inb(g_nabm_bar, AC97_PO_CR);

    while (written < len) {
        u8 civ = ac97_inb(g_nabm_bar, AC97_PO_CIV);
        u8 target_slot;

        if (!g_ac97_started || !(cr & 0x01)) {
            /* Playback stopped: start writing directly at CIV */
            target_slot = civ;
            g_ac97_lvi = civ;
            g_ac97_started = true;
        } else {
            /* Playback running: write into next buffer slot */
            target_slot = (g_ac97_lvi + 1) % AC97_BDL_ENTRIES;
            if (target_slot == civ) {
                /* Ring buffer full */
                spinlock_unlock_irqrestore(&g_ac97_lock, irqf);
                return (s64)written;
            }
        }

        u64 chunk = AC97_BUFFER_SIZE;
        if (len - written < chunk) chunk = len - written;

        __builtin_memcpy(g_audio_buffers[target_slot], data + written, chunk);
        
        g_bdl[target_slot].ptr = (u32)VIRT_TO_PHYS((virt_addr_t)g_audio_buffers[target_slot]);
        g_bdl[target_slot].samples = (chunk / 2);
        g_bdl[target_slot].flags = AC97_BDL_IOC;

        g_ac97_lvi = target_slot;
        ac97_outb(g_nabm_bar, AC97_PO_LVI, g_ac97_lvi);
        
        cr = ac97_inb(g_nabm_bar, AC97_PO_CR);
        if (!(cr & 0x01)) {
            ac97_outb(g_nabm_bar, AC97_PO_CR, cr | 0x01);
        }

        written += chunk;
    }

    spinlock_unlock_irqrestore(&g_ac97_lock, irqf);
    return (s64)written;
}

static s64 ac97_ioctl(u64 cmd, void *arg)
{
    spinlock_lock(&g_ac97_lock);
    if (!g_nam_bar) {
        spinlock_unlock(&g_ac97_lock);
        return -1;
    }

    switch (cmd) {
    case SOUND_PCM_WRITE_VOLUME: {
        if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) { spinlock_unlock(&g_ac97_lock); return -1; }
        u32 vol;
        if (copy_from_user(&vol, arg, sizeof(u32)) != 0) { spinlock_unlock(&g_ac97_lock); return -1; }
        u8 left = (vol & 0xFF) * 31 / 100;
        u8 right = ((vol >> 8) & 0xFF) * 31 / 100;
        u16 raw_vol = ((31 - (left & 0x1F)) << 8) | (31 - (right & 0x1F));
        ac97_outw(g_nam_bar, AC97_NAMBAR_MASTER_VOL, raw_vol);
        ac97_outw(g_nam_bar, AC97_NAMBAR_PCM_OUT_VOL, raw_vol);
        spinlock_unlock(&g_ac97_lock);
        return 0;
    }
    case SOUND_PCM_WRITE_RATE: {
        if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) { spinlock_unlock(&g_ac97_lock); return -1; }
        u32 rate;
        if (copy_from_user(&rate, arg, sizeof(u32)) != 0) { spinlock_unlock(&g_ac97_lock); return -1; }
        ac97_outw(g_nam_bar, AC97_NAMBAR_PCM_FRONT_RATE, (u16)rate);
        spinlock_unlock(&g_ac97_lock);
        return 0;
    }
    default:
        spinlock_unlock(&g_ac97_lock);
        return -1;
    }
}

static sound_ops_t g_ac97_ops = {
    .write_pcm = ac97_write_pcm,
    .ioctl = ac97_ioctl
};

static void ac97_scan_tree(device_t *node)
{
    if (!node) return;

    pci_device_info_t *pci = pci_get_device_info(node);
    if (pci) {
        if (pci->vendor_id == 0x8086 && pci->device_id == 0x2415) {
            pr_debug("[AC97] Found Intel AC97 at PCI %02x:%02x.%x\n",
                     pci->bus, pci->slot, pci->func);

            u16 cmd = pci_config_read16(pci->bus, pci->slot, pci->func, 0x04);
            pci_config_write16(pci->bus, pci->slot, pci->func, 0x04, cmd | 0x05);

            g_nam_bar  = pci_get_bar(node, 0) & ~1;
            g_nabm_bar = pci_get_bar(node, 1) & ~1;
            g_ac97_irq = pci_config_read8(pci->bus, pci->slot, pci->func, 0x3C);

            if (!g_nam_bar || !g_nabm_bar) {
                pr_debug("[AC97] Error: Invalid BARs.\n");
                return;
            }

            idt_register_irq(g_ac97_irq + 32, ac97_irq_handler, NULL);
            hal_irq_enable(g_ac97_irq, g_ac97_irq + 32);

            ac97_outd(g_nabm_bar, 0x2C, 0x02); /* Reset */
            for(volatile int i=0; i<100000; i++);

            ac97_outw(g_nam_bar, AC97_NAMBAR_MASTER_VOL, 0x0000);
            ac97_outw(g_nam_bar, AC97_NAMBAR_PCM_OUT_VOL, 0x0000);

            /* Allocate 32-bit low physical memory for DMA structures */
            phys_addr_t bdl_phys = pmm_alloc_pages_32(1);
            if (!bdl_phys) return;
            g_bdl = (ac97_bdl_entry_t *)PHYS_TO_VIRT(bdl_phys);

            bool alloc_ok = true;
            for (int i = 0; i < AC97_BDL_ENTRIES; i++) {
                phys_addr_t buf_phys = pmm_alloc_pages_32(1);
                if (!buf_phys) {
                    alloc_ok = false;
                    break;
                }
                g_audio_buffers[i] = (u8 *)PHYS_TO_VIRT(buf_phys);
                g_bdl[i].ptr = (u32)buf_phys;
                g_bdl[i].samples = 0;
                g_bdl[i].flags = 0;
            }

            if (!alloc_ok) {
                pr_debug("[AC97] Failed to allocate low 32-bit DMA buffers.\n");
                return;
            }

            ac97_outd(g_nabm_bar, AC97_PO_BDBAR, (u32)bdl_phys);
            ac97_outb(g_nabm_bar, AC97_PO_LVI, 0);
            ac97_outb(g_nabm_bar, AC97_PO_CR, 0x00);
            g_ac97_lvi = 0;
            g_ac97_started = false;

            pr_debug("[AC97] Initialized on IRQ %d. NAM: 0x%x, NABM: 0x%x\n", g_ac97_irq, g_nam_bar, g_nabm_bar);

            __builtin_memcpy(g_ac97_sound_dev.name, "Intel AC97", 11);
            g_ac97_sound_dev.ops = &g_ac97_ops;
            sound_register_device(&g_ac97_sound_dev);
            
            return;
        }
    }

    device_t *child = node->children;
    while (child) {
        ac97_scan_tree(child);
        child = child->sibling;
    }
}

void ac97_init(void)
{
    pr_debug("[AC97] Probing for Intel AC97...\n");
    ac97_scan_tree(device_tree_root());
}
