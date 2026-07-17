/**
 * virtio_gpu.c — AzamiOS Virtio-GPU 2D Display Driver
 *
 * Implements the Virtio 1.x GPU device (PCI device 0x1050, vendor 0x1AF4)
 * using the legacy virtio PCI I/O interface (BAR0), with 2D scanout via
 * RESOURCE_CREATE_2D → ATTACH_BACKING → SET_SCANOUT → TRANSFER → FLUSH.
 */
#include "../include/virtio_gpu.h"
#include "../include/virtio.h"
#include "../include/pci.h"
#include "../../klibc/include/port.h"
#include "../../klibc/include/stdio.h"
#include "../../klibc/include/string.h"
#include "../../mem/include/pmm.h"
#include "../../mem/include/paging.h"

/* ── Virtio PCI legacy register offsets (from BAR0 I/O base) ─────────────── */
#define VREG_DEVICE_FEATURES 0x00   /* R  device feature bits */
#define VREG_GUEST_FEATURES  0x04   /* RW driver negotiated features */
#define VREG_QUEUE_ADDR      0x08   /* RW queue physical page (>>12) */
#define VREG_QUEUE_SIZE      0x0C   /* R  queue size */
#define VREG_QUEUE_SEL       0x0E   /* RW select queue */
#define VREG_QUEUE_NOTIFY    0x10   /* W  notify device */
#define VREG_DEVICE_STATUS   0x12   /* RW device status */
#define VREG_ISR_STATUS      0x13   /* R  ISR status */
#define VREG_DEVICE_CONFIG   0x14   /* device-specific config starts here */

/* Virtio-GPU device config offsets (relative to VREG_DEVICE_CONFIG) */
#define VGPU_CFG_EVENTS_READ  0x00
#define VGPU_CFG_EVENTS_CLEAR 0x04
#define VGPU_CFG_NUM_SCANOUTS 0x08

/* ── Virtio GPU command types ─────────────────────────────────────────────── */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO        0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF          0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT             0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH          0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107

#define VIRTIO_GPU_RESP_OK_NODATA              0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO        0x1101
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM       1

/* ── Virtq (virtio split ring) minimal implementation ───────────────────── */
#define VIRTQ_SIZE 16  /* must be power of two; 16 is plenty for GPU */

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTQ_SIZE];
    uint16_t event;
} virtq_avail_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t len;
} virtq_used_elem_t;

typedef struct __attribute__((packed)) {
    uint16_t           flags;
    uint16_t           idx;
    virtq_used_elem_t  ring[VIRTQ_SIZE];
    uint16_t           avail_event;
} virtq_used_t;

typedef struct {
    virtq_desc_t  *desc;
    virtq_avail_t *avail;
    virtq_used_t  *used;
    uint16_t       last_used;
    uint16_t       next_desc;
} virtq_t;

/* ── GPU control/cursor structures ───────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} vgpu_ctrl_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t x, y, width, height;
} vgpu_rect_t;

typedef struct __attribute__((packed)) {
    vgpu_ctrl_hdr_t hdr;
    struct {
        vgpu_rect_t  r;
        uint32_t     enabled;
        uint32_t     flags;
        uint32_t     virgl_width;   /* padding */
        uint32_t     virgl_height;
    } pmodes[16];
} vgpu_resp_display_info_t;

typedef struct __attribute__((packed)) {
    vgpu_ctrl_hdr_t hdr;
    uint32_t        format;
    uint32_t        resource_id;
    uint32_t        width;
    uint32_t        height;
} vgpu_cmd_create_2d_t;

typedef struct __attribute__((packed)) {
    vgpu_ctrl_hdr_t hdr;
    uint32_t        scanout_id;
    uint32_t        resource_id;
    vgpu_rect_t     r;
} vgpu_cmd_set_scanout_t;

typedef struct __attribute__((packed)) {
    vgpu_ctrl_hdr_t hdr;
    uint32_t        resource_id;
    uint32_t        nr_entries;
} vgpu_cmd_attach_backing_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} vgpu_mem_entry_t;

typedef struct __attribute__((packed)) {
    vgpu_ctrl_hdr_t hdr;
    vgpu_rect_t     r;
    uint64_t        offset;
    uint32_t        resource_id;
    uint32_t        padding;
} vgpu_cmd_transfer_t;

typedef struct __attribute__((packed)) {
    vgpu_ctrl_hdr_t hdr;
    vgpu_rect_t     r;
    uint32_t        resource_id;
    uint32_t        padding;
} vgpu_cmd_flush_t;

/* ── Driver state ────────────────────────────────────────────────────────── */

static struct {
    uint16_t io_base;
    virtq_t  ctrlq;

    /* Framebuffer backing */
    uint32_t *fb_virt;       /* virtual (identity mapped) */
    uint32_t  fb_phys;       /* physical */
    uint32_t  fb_size;

    uint32_t  width, height;
    uint32_t  resource_id;
    bool      active;
} g_vgpu;

/* ── I/O helpers ─────────────────────────────────────────────────────────── */

static inline uint32_t vio_inl(uint16_t reg) { return inl((uint16_t)(g_vgpu.io_base + reg)); }
static inline uint16_t vio_inw(uint16_t reg) { return inw((uint16_t)(g_vgpu.io_base + reg)); }
static inline uint8_t  vio_inb(uint16_t reg) { return inb((uint16_t)(g_vgpu.io_base + reg)); }
static inline void vio_outl(uint16_t reg, uint32_t v) { outl((uint16_t)(g_vgpu.io_base + reg), v); }
static inline void vio_outw(uint16_t reg, uint16_t v) { outw((uint16_t)(g_vgpu.io_base + reg), v); }
static inline void vio_outb(uint16_t reg, uint8_t  v) { outb((uint16_t)(g_vgpu.io_base + reg), v); }

/* ── Virtqueue helpers ───────────────────────────────────────────────────── */

static int virtq_setup(virtq_t *q, uint16_t queue_sel) {
    vio_outw(VREG_QUEUE_SEL, queue_sel);
    uint16_t qsz = vio_inw(VREG_QUEUE_SIZE);
    if (!qsz) return -1;

    /* One 4K page is enough for desc + avail + used with VIRTQ_SIZE=16 */
    void *page = pmm_alloc_block();
    if (!page) return -1;
    memset(page, 0, 4096);

    uintptr_t base = (uintptr_t)page;
    q->desc      = (virtq_desc_t *) base;
    q->avail     = (virtq_avail_t *)(base + VIRTQ_SIZE * sizeof(virtq_desc_t));
    q->used      = (virtq_used_t *) (base + 4096 / 2);
    q->last_used = 0;
    q->next_desc = 0;

    vio_outl(VREG_QUEUE_ADDR, (uint32_t)((uintptr_t)page >> 12));
    return 0;
}

/* Send a two-buffer command (request + response) and wait for completion. */
static int virtq_send_cmd(virtq_t *q, uint16_t qidx,
                           void *cmd, uint32_t cmd_len,
                           void *resp, uint32_t resp_len) {
    uint16_t d0 = (uint16_t)(q->next_desc % VIRTQ_SIZE);
    uint16_t d1 = (uint16_t)((q->next_desc + 1) % VIRTQ_SIZE);
    q->next_desc = (uint16_t)((q->next_desc + 2) % VIRTQ_SIZE);

    /* Descriptor 0: command (device reads) */
    q->desc[d0].addr  = (uint64_t)(uintptr_t)cmd;
    q->desc[d0].len   = cmd_len;
    q->desc[d0].flags = 0x01;  /* NEXT */
    q->desc[d0].next  = d1;

    /* Descriptor 1: response (device writes) */
    q->desc[d1].addr  = (uint64_t)(uintptr_t)resp;
    q->desc[d1].len   = resp_len;
    q->desc[d1].flags = 0x02;  /* WRITE */
    q->desc[d1].next  = 0;

    /* Publish to available ring */
    uint16_t avail_idx = q->avail->idx % VIRTQ_SIZE;
    q->avail->ring[avail_idx] = d0;
    asm volatile("" ::: "memory");
    q->avail->idx++;
    asm volatile("" ::: "memory");

    /* Notify device */
    vio_outw(VREG_QUEUE_NOTIFY, qidx);

    /* Poll used ring for completion (busy-wait) */
    for (int i = 0; i < 1000000; i++) {
        asm volatile("" ::: "memory");
        if (q->used->idx != q->last_used) {
            q->last_used++;
            return 0;
        }
    }
    kprintf("virtio-gpu: command timeout\n");
    return -1;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

bool virtio_gpu_is_active(void) { return g_vgpu.active; }
uint32_t virtio_gpu_width(void) { return g_vgpu.width; }
uint32_t virtio_gpu_height(void) { return g_vgpu.height; }

bool virtio_gpu_init(void) {
    pci_device_t *pdev = pci_find_vendor(VIRTIO_VENDOR_ID, 0x1050);
    if (!pdev) {
        /* Also check modern GPU ID */
        pdev = pci_find_vendor(VIRTIO_VENDOR_ID, 0x1040 + 16);
    }
    if (!pdev) {
        kprintf("virtio-gpu: no GPU device found\n");
        return false;
    }

    uint32_t bar0 = pdev->bar[0];
    if (!(bar0 & 1)) {
        kprintf("virtio-gpu: BAR0 is MMIO, need I/O BAR\n");
        return false;
    }
    g_vgpu.io_base = (uint16_t)(bar0 & ~3u);
    pci_enable_busmaster(pdev);

    kprintf("virtio-gpu: PCI %u:%u.%u I/O=0x%x IRQ=%u\n",
            pdev->bus, pdev->slot, pdev->func, g_vgpu.io_base, pdev->irq);

    /* Virtio init sequence: reset → ACK → DRIVER */
    vio_outb(VREG_DEVICE_STATUS, 0);
    vio_outb(VREG_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Negotiate features: we don't need any GPU-specific features for 2D */
    uint32_t features = vio_inl(VREG_DEVICE_FEATURES);
    features &= ~(1u << 1);  /* Clear VIRTIO_GPU_F_VIRGL */
    vio_outl(VREG_GUEST_FEATURES, features);
    vio_outb(VREG_DEVICE_STATUS,
             VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
             VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    /* Set up controlq (queue 0) */
    if (virtq_setup(&g_vgpu.ctrlq, 0) != 0) {
        kprintf("virtio-gpu: ctrlq setup failed\n");
        return false;
    }

    /* GET_DISPLAY_INFO */
    static vgpu_ctrl_hdr_t         req_info;
    static vgpu_resp_display_info_t resp_info;
    memset(&req_info, 0, sizeof(req_info));
    memset(&resp_info, 0, sizeof(resp_info));
    req_info.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    virtq_send_cmd(&g_vgpu.ctrlq, 0,
                   &req_info, sizeof(req_info),
                   &resp_info, sizeof(resp_info));

    g_vgpu.width  = resp_info.pmodes[0].r.width;
    g_vgpu.height = resp_info.pmodes[0].r.height;
    if (!g_vgpu.width || !g_vgpu.height) {
        g_vgpu.width  = 1280;
        g_vgpu.height = 800;
    }
    kprintf("virtio-gpu: display %ux%u\n", g_vgpu.width, g_vgpu.height);

    /* Allocate framebuffer backing (contiguous pages via PMM) */
    g_vgpu.fb_size = g_vgpu.width * g_vgpu.height * 4;
    uint32_t npages = (g_vgpu.fb_size + 4095) / 4096;
    void *fb_phys_ptr = (void*)0;
    for (uint32_t i = 0; i < npages; i++) {
        void *p = pmm_alloc_block();
        if (!p) { kprintf("virtio-gpu: OOM for framebuffer\n"); return false; }
        if (i == 0) fb_phys_ptr = p;
        paging_map_page((uintptr_t)p, (uintptr_t)p, 1, 1);
    }
    g_vgpu.fb_phys = (uint32_t)(uintptr_t)fb_phys_ptr;
    g_vgpu.fb_virt = (uint32_t *)(uintptr_t)fb_phys_ptr;
    memset(g_vgpu.fb_virt, 0, g_vgpu.fb_size);

    g_vgpu.resource_id = 1;

    /* RESOURCE_CREATE_2D */
    static vgpu_cmd_create_2d_t req_create;
    static vgpu_ctrl_hdr_t      resp_create;
    memset(&req_create, 0, sizeof(req_create));
    req_create.hdr.type   = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    req_create.format      = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    req_create.resource_id = g_vgpu.resource_id;
    req_create.width       = g_vgpu.width;
    req_create.height      = g_vgpu.height;
    virtq_send_cmd(&g_vgpu.ctrlq, 0,
                   &req_create, sizeof(req_create),
                   &resp_create, sizeof(resp_create));

    /* RESOURCE_ATTACH_BACKING */
    static struct {
        vgpu_cmd_attach_backing_t cmd;
        vgpu_mem_entry_t          entry;
    } req_attach;
    static vgpu_ctrl_hdr_t resp_attach;
    memset(&req_attach, 0, sizeof(req_attach));
    req_attach.cmd.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    req_attach.cmd.resource_id = g_vgpu.resource_id;
    req_attach.cmd.nr_entries  = 1;
    req_attach.entry.addr      = (uint64_t)g_vgpu.fb_phys;
    req_attach.entry.length    = g_vgpu.fb_size;
    virtq_send_cmd(&g_vgpu.ctrlq, 0,
                   &req_attach, sizeof(req_attach),
                   &resp_attach, sizeof(resp_attach));

    /* SET_SCANOUT */
    static vgpu_cmd_set_scanout_t req_scanout;
    static vgpu_ctrl_hdr_t        resp_scanout;
    memset(&req_scanout, 0, sizeof(req_scanout));
    req_scanout.hdr.type   = VIRTIO_GPU_CMD_SET_SCANOUT;
    req_scanout.scanout_id = 0;
    req_scanout.resource_id = g_vgpu.resource_id;
    req_scanout.r.x = 0; req_scanout.r.y = 0;
    req_scanout.r.width  = g_vgpu.width;
    req_scanout.r.height = g_vgpu.height;
    virtq_send_cmd(&g_vgpu.ctrlq, 0,
                   &req_scanout, sizeof(req_scanout),
                   &resp_scanout, sizeof(resp_scanout));

    g_vgpu.active = true;
    kprintf("virtio-gpu: 2D scanout active (%ux%u BGRX)\n",
            g_vgpu.width, g_vgpu.height);
    return true;
}

void virtio_gpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!g_vgpu.active) return;

    static vgpu_cmd_transfer_t req_xfer;
    static vgpu_ctrl_hdr_t     resp_xfer;
    memset(&req_xfer, 0, sizeof(req_xfer));
    req_xfer.hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    req_xfer.resource_id = g_vgpu.resource_id;
    req_xfer.r.x = x; req_xfer.r.y = y;
    req_xfer.r.width = w; req_xfer.r.height = h;
    req_xfer.offset = (uint64_t)(y * g_vgpu.width + x) * 4;
    virtq_send_cmd(&g_vgpu.ctrlq, 0,
                   &req_xfer, sizeof(req_xfer),
                   &resp_xfer, sizeof(resp_xfer));

    static vgpu_cmd_flush_t req_flush;
    static vgpu_ctrl_hdr_t  resp_flush;
    memset(&req_flush, 0, sizeof(req_flush));
    req_flush.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    req_flush.resource_id = g_vgpu.resource_id;
    req_flush.r.x = x; req_flush.r.y = y;
    req_flush.r.width = w; req_flush.r.height = h;
    virtq_send_cmd(&g_vgpu.ctrlq, 0,
                   &req_flush, sizeof(req_flush),
                   &resp_flush, sizeof(resp_flush));
}

void virtio_gpu_blit(const uint32_t *pixels, uint32_t w, uint32_t h) {
    if (!g_vgpu.active || !pixels) return;
    uint32_t copy_w = w < g_vgpu.width  ? w : g_vgpu.width;
    uint32_t copy_h = h < g_vgpu.height ? h : g_vgpu.height;
    for (uint32_t row = 0; row < copy_h; row++) {
        uintptr_t cnt = copy_w;
        const uint32_t *src  = pixels + row * w;
              uint32_t *dst  = g_vgpu.fb_virt + row * g_vgpu.width;
        asm volatile("rep movsl"
                     : "+S"(src), "+D"(dst), "+c"(cnt)
                     : : "memory");
    }
    virtio_gpu_flush(0, 0, copy_w, copy_h);
}
