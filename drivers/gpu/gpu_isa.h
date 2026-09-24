/* ============================================================================
 * AzamiOS — GPU Hardware Instruction Set Reference
 * File: drivers/gpu/gpu_isa.h
 *
 * Central reference for every GPU hardware backend AzamiOS supports.
 * This header is *documentation and constant definitions only* — it never
 * touches live hardware itself.  Each backend driver (bga.c, vmwgfx_drv.c,
 * virtgpu_drm.c) includes or mirrors the subset it uses; this file collects
 * all of them in one place so a developer adding a new driver (or debugging
 * an existing one) can read the full picture without hunting across three
 * files.
 *
 * Backends covered:
 *   BGA    — Bochs/QEMU VBE-compatible adapter (I/O-port programmed)
 *   SVGA   — VMware SVGA II / SVGA3D (I/O-port + MMIO FIFO 2D/3D pipeline)
 *   VGPU   — VirtIO-GPU & Virgl 3D (virtqueue-based 2D/3D paravirtual device with TGSI shader ISA)
 *   INTEL  — Intel Gen / Iris Graphics Command Streamer (Ring buffer, MI, 3DSTATE, EU SIMD ISA)
 *   RADEON — AMD Radeon / RDNA / GCN Command Processor (CP Packet 3, Wavefront execution ISA)
 *
 * I/O discipline for write-combining MMIO apertures
 * ──────────────────────────────────────────────────
 * All three backends expose a linear framebuffer in BAR-space that the
 * firmware configures as Write-Combining (WC).  WC coalesces stores into
 * PCIe bursts but does not guarantee ordering with ordinary stores.  The
 * rule is:
 *
 *   1.  Write pixels with MOVNTI (non-temporal) or ordinary stores to the WC
 *       mapping — never with cached loads (PREFETCHNTA is fine, PREFETCHT0
 *       is wasteful because it fills the L1 with data that belongs on the
 *       bus, not in the cache).
 *
 *   2.  After the last pixel store, emit SFENCE before any control-register
 *       write (SVGA_REG_SYNC, RESOURCE_FLUSH virtqueue entry, BGA Y_OFFSET)
 *       that asks the device to present the frame.  Without it, the device
 *       may observe the control write before the final pixel stores drain
 *       from the WC buffer.
 *
 *   3.  Never issue CLFLUSH/CLFLUSHOPT against a WC mapping — it is a no-op
 *       at best and a cache-pollution hazard at worst.
 *
 * These rules are already implemented in hw_copy_to_vram() / hw_fill_vram()
 * (arch/x86_64/cpu/hwaccel.h); call those rather than open-coding stores.
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"

/* ════════════════════════════════════════════════════════════════════════════
 * §1  BOCHS GRAPHICS ADAPTER (BGA / VBE DISPI)
 *
 * The Bochs Graphics Adapter is the de-facto standard emulated VGA device
 * for QEMU (-vga std), VirtualBox, and Bochs.  It is programmed through a
 * pair of 16-bit I/O ports:
 *
 *   VBE_DISPI_IOPORT_INDEX  0x01CE  ← register selector (write)
 *   VBE_DISPI_IOPORT_DATA   0x01CF  ← register value    (read/write)
 *
 * A mode change sequence is always:
 *   1. Write VBE_DISPI_DISABLED to INDEX_ENABLE
 *   2. Write the desired XRES, YRES, BPP, VIRT_WIDTH, VIRT_HEIGHT
 *   3. Write VBE_DISPI_ENABLED | flags to INDEX_ENABLE
 *
 * The adapter never raises an interrupt; all synchronisation is polling.
 * ════════════════════════════════════════════════════════════════════════════ */

/* ── I/O Ports ─────────────────────────────────────────────────────────────── */
#define BGA_IOPORT_INDEX        0x01CE
#define BGA_IOPORT_DATA         0x01CF

/* ── Register Indices ──────────────────────────────────────────────────────── */
#define BGA_REG_ID              0x00    /* Device version ID (read-only)         */
#define BGA_REG_XRES            0x01    /* Horizontal display resolution         */
#define BGA_REG_YRES            0x02    /* Vertical display resolution           */
#define BGA_REG_BPP             0x03    /* Bits per pixel (4/8/15/16/24/32)      */
#define BGA_REG_ENABLE          0x04    /* Enable/disable + feature flags        */
#define BGA_REG_BANK            0x05    /* Bank index (legacy; 0 in LFB mode)    */
#define BGA_REG_VIRT_WIDTH      0x06    /* Virtual canvas width (≥ XRES)         */
#define BGA_REG_VIRT_HEIGHT     0x07    /* Virtual canvas height (≥ YRES)        */
#define BGA_REG_X_OFFSET        0x08    /* Horizontal scroll offset in the canvas*/
#define BGA_REG_Y_OFFSET        0x09    /* Vertical scroll offset — hardware flip*/
#define BGA_REG_VIDEO_MEMORY_64K 0x0A  /* VRAM size in 64 KiB units (read-only) */

/* ── Known Device IDs ─────────────────────────────────────────────────────── */
#define BGA_ID0                 0xB0C0  /* Bochs ≤ 1.x (no LFB)                 */
#define BGA_ID1                 0xB0C1
#define BGA_ID2                 0xB0C2
#define BGA_ID3                 0xB0C3
#define BGA_ID4                 0xB0C4  /* LFB present (BAR0)                    */
#define BGA_ID5                 0xB0C5  /* Virtual width/height support          */

/* ── BGA_REG_ENABLE bit-field ─────────────────────────────────────────────── */
#define BGA_ENABLE_DISABLED     0x00    /* Adapter off                           */
#define BGA_ENABLE_ENABLED      0x01    /* Adapter active                        */
#define BGA_ENABLE_GETCAPS      0x02    /* Read max W/H/BPP from XRES/YRES/BPP  */
#define BGA_ENABLE_8BIT_DAC     0x20    /* 8-bit DAC (palette mode)              */
#define BGA_ENABLE_LFB          0x40    /* Map framebuffer at BAR0               */
#define BGA_ENABLE_NOCLEARMEM   0x80    /* Skip screen clear on mode change      */

/* ── Aperture size the driver maps into kernel virtual space ─────────────── */
#define BGA_APERTURE_SIZE       (16u * 1024u * 1024u)   /* 16 MiB               */

/* ── Page-flip protocol ─────────────────────────────────────────────────────
 * The virtual canvas is set to 2× YRES.  Page 0 lives at Y=0, page 1 at
 * Y=YRES.  Flipping is a single Y_OFFSET write:
 *
 *   write(BGA_REG_Y_OFFSET, buffer_index * height);
 *
 * This is the only zero-copy flip path on BGA; all pixel work happens in the
 * invisible half before the write.  The write is not atomic with in-flight
 * WC stores, so flush the WC buffer first with SFENCE (hw_sfence() in
 * hwaccel.h) before issuing the Y_OFFSET write.
 * ──────────────────────────────────────────────────────────────────────────── */


/* ════════════════════════════════════════════════════════════════════════════
 * §2  VMWARE SVGA II
 *
 * The VMware SVGA II adapter (PCI 15AD:0405) is programmed through:
 *   BAR0 — I/O-port index/value pair  (SVGA_INDEX_PORT / SVGA_VALUE_PORT)
 *   BAR1 — linear framebuffer MMIO
 *   BAR2 — command FIFO MMIO
 *
 * Register access:
 *   outl(BAR0 + SVGA_INDEX_PORT, SVGA_REG_*);
 *   value = inl(BAR0 + SVGA_VALUE_PORT);
 *
 * Initialisation sequence:
 *   1.  Negotiate version: write SVGA_ID_2, read back; decrement until match.
 *   2.  Read SVGA_REG_FB_START, SVGA_REG_VRAM_SIZE, SVGA_REG_MEM_START,
 *       SVGA_REG_MEM_SIZE.  Map BAR1 and BAR2.
 *   3.  Initialise the FIFO: write FIFO_MIN, FIFO_MAX, FIFO_NEXT_CMD, FIFO_STOP.
 *   4.  Write SVGA_REG_CONFIG_DONE = 1 to tell the device the FIFO is ready.
 *   5.  Write desired width/height/bpp and SVGA_REG_ENABLE = 1.
 *   6.  Read back SVGA_REG_BYTES_PER_LINE (pitch) — it may differ from W×(bpp/8).
 * ════════════════════════════════════════════════════════════════════════════ */

/* ── BAR0 port offsets ─────────────────────────────────────────────────────── */
#define SVGA_INDEX_PORT         0       /* Register selector (32-bit write)      */
#define SVGA_VALUE_PORT         1       /* Register data     (32-bit read/write) */

/* ── Register map ─────────────────────────────────────────────────────────── */
#define SVGA_REG_ID             0       /* Magic + version (SVGA_ID_*)           */
#define SVGA_REG_ENABLE         1       /* 0 = off, 1 = on                       */
#define SVGA_REG_WIDTH          2       /* Horizontal display resolution         */
#define SVGA_REG_HEIGHT         3       /* Vertical display resolution           */
#define SVGA_REG_MAX_WIDTH      4       /* Maximum supported width  (read-only)  */
#define SVGA_REG_MAX_HEIGHT     5       /* Maximum supported height (read-only)  */
#define SVGA_REG_DEPTH          6       /* Colour depth (alias of BITS_PER_PIXEL)*/
#define SVGA_REG_BITS_PER_PIXEL 7       /* Bits per pixel                        */
#define SVGA_REG_PSEUDOCOLOR    8       /* 1 = indexed palette mode              */
#define SVGA_REG_RED_MASK       9       /* Red channel mask (read-only)          */
#define SVGA_REG_GREEN_MASK     10      /* Green channel mask (read-only)        */
#define SVGA_REG_BLUE_MASK      11      /* Blue channel mask (read-only)         */
#define SVGA_REG_BYTES_PER_LINE 12      /* Pitch in bytes (read after enable)    */
#define SVGA_REG_FB_START       13      /* Physical base of BAR1 (read-only)     */
#define SVGA_REG_FB_OFFSET      14      /* Byte offset from FB_START to scanout  */
#define SVGA_REG_VRAM_SIZE      15      /* Total VRAM size in bytes (read-only)  */
#define SVGA_REG_FB_SIZE        16      /* Usable framebuffer size (read-only)   */
#define SVGA_REG_CAPABILITIES   17      /* Feature capability bits (read-only)   */
#define SVGA_REG_MEM_START      18      /* Physical base of FIFO (read-only)     */
#define SVGA_REG_MEM_SIZE       19      /* FIFO size in bytes (read-only)        */
#define SVGA_REG_CONFIG_DONE    20      /* Write 1 after FIFO init to start      */
#define SVGA_REG_SYNC           21      /* Write any value to flush + sync       */
#define SVGA_REG_BUSY           22      /* Non-zero while device is processing   */
#define SVGA_REG_GUEST_ID       23      /* Guest OS identifier (optional)        */
/* ── Hardware cursor registers (SVGA_ID_2, requires SVGA_CAP_CURSOR) ─────── */
#define SVGA_REG_CURSOR_ON      27      /* 0 = hidden, 1 = visible               */
    /* Write CURSOR_ON = 1 AFTER uploading the cursor via DEFINE_ALPHA_CURSOR.
     * Write CURSOR_ON = 0 to hide.  vmwgfx_drv.c uses this ordering. */
#define SVGA_REG_CURSOR_X       28      /* Cursor hot-spot X position on screen  */
#define SVGA_REG_CURSOR_Y       29      /* Cursor hot-spot Y position on screen  */
#define SVGA_REG_CURSOR_ID      30      /* Hardware cursor shape ID (u32)        */
    /* Write CURSOR_ID before CURSOR_ON; the device loads the shape first.
     * IDs are chosen by the driver; QEMU/VMware both accept any u32. */
#define SVGA_REG_HOST_BITS_PER_PIXEL 31 /* Host display depth (read-only)       */
#define SVGA_REG_SCRATCH_SIZE   32      /* Scratch register count (read-only)    */
#define SVGA_REG_MEM_REGS       33      /* FIFO register count (read-only)       */

/* ── Version handshake ────────────────────────────────────────────────────── */
#define SVGA_MAGIC              0x900000UL
#define SVGA_MAKE_ID(v)         ((u32)(((SVGA_MAGIC) << 8) | (v)))
#define SVGA_ID_0               SVGA_MAKE_ID(0)
#define SVGA_ID_1               SVGA_MAKE_ID(1)
#define SVGA_ID_2               SVGA_MAKE_ID(2)     /* Minimum usable version    */

/* ── FIFO layout — first four u32 slots are the FIFO's own control words ─── */
#define SVGA_FIFO_MIN           0       /* Byte offset to first command slot     */
#define SVGA_FIFO_MAX           1       /* Byte offset one past the last slot    */
#define SVGA_FIFO_NEXT_CMD      2       /* Producer index (driver writes here)   */
#define SVGA_FIFO_STOP          3       /* Consumer index (device updates this)  */
#define SVGA_FIFO_NUM_REGS      4       /* First 4 words are FIFO control        */

/* ── FIFO command opcodes ─────────────────────────────────────────────────── */
#define SVGA_CMD_UPDATE                 1
    /* Args: x, y, width, height (all u32)
     * Tells the device to repaint a rectangle from the framebuffer.
     * Must be followed by SVGA_REG_SYNC to drain the FIFO. */

#define SVGA_CMD_RECT_FILL              2
    /* Fill a solid-colour rectangle in the framebuffer WITHOUT going through
     * the host compositor — the device writes the VRAM directly.
     *
     * Argument layout (5 u32 words, written with svga_fifo_write()):
     *   [0]  SVGA_CMD_RECT_FILL  (the opcode itself)
     *   [1]  color   — packed pixel value in the framebuffer's native format:
     *                  0x00RRGGBB for 32bpp (alpha ignored by hardware)
     *   [2]  x       — left edge of rectangle in pixels
     *   [3]  y       — top  edge of rectangle in pixels
     *   [4]  width   — rectangle width  in pixels
     *   [5]  height  — rectangle height in pixels
     *
     * The fill runs at device speed (hardware accelerated on real VMware;
     * emulated by QEMU but still faster than a guest-side memset because the
     * traffic stays on the host side of the virtqueue).  Must be followed by
     * SVGA_REG_SYNC to ensure the device sees it before the next flip.
     *
     * Only valid when SVGA_CAP_RECT_FILL is set in SVGA_REG_CAPABILITIES.
     * The AzamiOS vmwgfx driver checks this before emitting the command. */

#define SVGA_CMD_RECT_COPY              3
    /* Copy a rectangle within the framebuffer on the host side.
     * Used for scrolling: instead of re-blitting the entire shadow buffer,
     * the driver moves an existing region by one command and only blits the
     * newly exposed strip.
     *
     * Argument layout (7 u32 words):
     *   [0]  SVGA_CMD_RECT_COPY  (the opcode itself)
     *   [1]  srcX    — source left  edge in pixels
     *   [2]  srcY    — source top   edge in pixels
     *   [3]  dstX    — dest   left  edge in pixels
     *   [4]  dstY    — dest   top   edge in pixels
     *   [5]  width   — region width  in pixels
     *   [6]  height  — region height in pixels
     *
     * The copy is always completed before the device processes any subsequent
     * SVGA_CMD_UPDATE, so a partial scroll followed immediately by an update
     * of the exposed strip will render correctly without an intervening SYNC.
     *
     * Only valid when SVGA_CAP_RECT_COPY is set in SVGA_REG_CAPABILITIES. */
    /* Args: src_x, src_y, dst_x, dst_y, width, height (all u32)
     * Copy a rectangle within the framebuffer.  Hardware-accelerated on real
     * VMware; emulated as a CPU copy in QEMU's implementation. */

#define SVGA_CMD_DEFINE_ALPHA_CURSOR   22
    /* Define a hardware alpha-blended cursor image.
     *
     * Argument layout:
     *   [0]  SVGA_CMD_DEFINE_ALPHA_CURSOR
     *   [1]  id          — cursor shape ID (any driver-chosen u32; written
     *                       to SVGA_REG_CURSOR_ID to select it)
     *   [2]  hotspotX    — X offset of the logical click point in pixels
     *   [3]  hotspotY    — Y offset of the logical click point in pixels
     *   [4]  width       — image width  in pixels (≤64 for broad compatibility)
     *   [5]  height      — image height in pixels (≤64 for broad compatibility)
     *   [6…N] pixels     — width*height ARGB8888 values, row-major, top-to-bottom
     *
     * After writing all words, call svga_sync() to let the device consume the
     * shape before writing SVGA_REG_CURSOR_ID and SVGA_REG_CURSOR_ON = 1.
     * Requires SVGA_CAP_ALPHA_CURSOR in SVGA_REG_CAPABILITIES. */

/* ── Capability bits (SVGA_REG_CAPABILITIES, read-only) ─────────────────────
 * These bits are present in the SVGA_ID_2 register set.  Read them once at
 * probe time and cache in the driver's device struct before emitting any
 * accelerated command — emitting RECT_FILL on a device without the cap bit
 * causes QEMU to log an "unhandled FIFO command" warning and stall the ring.
 * Real VMware hardware always implements the full set; QEMU only a subset. */
#define SVGA_CAP_NONE              0x00000000U
#define SVGA_CAP_RECT_FILL         0x00000001U /* SVGA_CMD_RECT_FILL available  */
#define SVGA_CAP_RECT_COPY         0x00000002U /* SVGA_CMD_RECT_COPY available  */
#define SVGA_CAP_RECT_PAT_FILL     0x00000004U /* pattern fill (not used here)  */
#define SVGA_CAP_LEGACY_OFFSCREEN  0x00000008U /* offscreen surfaces (unused)   */
#define SVGA_CAP_RASTER_OP         0x00000010U /* ROP3 blits (unused)           */
#define SVGA_CAP_CURSOR            0x00000020U /* software cursor overlay       */
#define SVGA_CAP_CURSOR_BYPASS     0x00000040U /* cursor bypasses update cmds   */
#define SVGA_CAP_CURSOR_BYPASS_2   0x00000080U /* cursor bypass v2              */
#define SVGA_CAP_8BIT_EMULATION    0x00000100U /* 8-bit pseudo-colour (unused)  */
#define SVGA_CAP_ALPHA_CURSOR      0x00000200U /* SVGA_CMD_DEFINE_ALPHA_CURSOR  */
    /* Required for the hardware cursor vmwgfx_drv.c uploads.
     * If this bit is clear the cursor must be composited in software. */
#define SVGA_CAP_3D                0x00004000U /* SVGA3D context support        */
#define SVGA_CAP_EXTENDED_FIFO     0x00008000U /* extra FIFO regs beyond [0..3] */
#define SVGA_CAP_MULTIMON          0x00010000U /* multiple scan-out heads       */
#define SVGA_CAP_PITCHLOCK         0x00020000U /* SVGA_REG_PITCHLOCK            */
#define SVGA_CAP_IRQMASK           0x00040000U /* SVGA_REG_IRQMASK              */
#define SVGA_CAP_DISPLAY_TOPOLOGY  0x00080000U /* multi-monitor layout info     */
#define SVGA_CAP_GMR               0x00100000U /* Guest Memory Regions          */
#define SVGA_CAP_TRACES            0x00200000U /* trace-based updates           */
#define SVGA_CAP_GMR2              0x00400000U /* Guest Memory Region v2        */
#define SVGA_CAP_SCREEN_OBJECT_2   0x00800000U /* SVGA3D screen object v2       */

#define SVGA_CMD_DEFINE_CURSOR          19
    /* Args: id (u32), hotspot_x, hotspot_y (u32), width, height (u32),
     *       andMaskDepth, xorMaskDepth (u32), AND mask pixels, XOR mask pixels.
     * Defines an AND-XOR hardware cursor shape. */

#define SVGA_CMD_DEFINE_ALPHA_CURSOR    22
    /* Args: id (u32), hotspot_x, hotspot_y (u32), width, height (u32),
     *       ARGB pixels[width * height].
     * Defines a full-ARGB alpha-blended hardware cursor. Preferred over
     * DEFINE_CURSOR on modern VMware/QEMU. */

#define SVGA_CMD_MOVE_CURSOR            29
    /* Args: pos_x, pos_y (u32)
     * Repositions the cursor without a FIFO sync — the device reads the new
     * position immediately from the FIFO, so a following UPDATE does not need
     * to wait for the cursor move. */

/* (SVGA_CAP_* macros defined above) */

/* ── FIFO sync / flush protocol ────────────────────────────────────────────
 * After writing one or more commands into the FIFO ring:
 *
 *   1.  sfence  — ensure all CPU stores to the WC FIFO mapping have left the
 *                 store buffer before the device can observe them.
 *   2.  Write SVGA_REG_SYNC = 1  — arms the device to process the new commands.
 *   3.  Poll SVGA_REG_BUSY until it reads 0 (bounded loop; emulated adapters
 *       clear it in one read).
 *
 * This three-step sequence is what svga_sync() in vmwgfx_drv.c implements.
 * ──────────────────────────────────────────────────────────────────────────── */


/* ════════════════════════════════════════════════════════════════════════════
 * §3  VIRTIO-GPU (paravirtual 2D/3D display device)
 *
 * The VirtIO-GPU device (PCI 1AF4:1050 modern, 1AF4:1010 legacy) uses two
 * virtqueues:
 *   Queue 0 — controlq: all 2D resource and scanout commands
 *   Queue 1 — cursorq:  UPDATE_CURSOR / MOVE_CURSOR (latency-critical path)
 *
 * Every command is a pair of descriptor chains:
 *   [driver→device]  header + command-specific struct (readable by device)
 *   [device→driver]  response header (writable by device)
 *
 * The driver fills the command buffer, adds the descriptor chain to the
 * available ring, kicks the queue, and then polls (or waits on interrupt) for
 * the used ring to advance.  The kernel implementation uses synchronous
 * polling; an OS with proper IRQ handling would use the interrupt path.
 *
 * Feature bits negotiated at init:
 *   VIRTIO_GPU_F_VIRGL (0) — host supports 3D (virglrenderer context)
 *   VIRTIO_GPU_F_EDID  (1) — host can return per-scanout EDID blobs
 *
 * Protocol version: this driver targets VIRTIO spec 1.1, virtio-gpu section.
 * ════════════════════════════════════════════════════════════════════════════ */

/* ── Feature bits ─────────────────────────────────────────────────────────── */
#define VGPU_F_VIRGL            0       /* 3D / virglrenderer context support    */
#define VGPU_F_EDID             1       /* Per-scanout EDID blob support         */

/* ── Control command opcodes ──────────────────────────────────────────────── */

/* 2D resource commands (controlq) */
#define VGPU_CMD_GET_DISPLAY_INFO       0x0100
    /* No additional payload after the header.
     * Response: virtio_gpu_resp_display_info — array of up to 16 scanout
     * descriptors (enabled flag + x/y/w/h rectangle). */

#define VGPU_CMD_RESOURCE_CREATE_2D     0x0101
    /* Payload: resource_id (u32), format (VGPU_FORMAT_*), width, height (u32).
     * Allocates a 2D resource on the host.  resource_id must be unique and
     * non-zero for the lifetime of the resource. */

#define VGPU_CMD_RESOURCE_UNREF         0x0102
    /* Payload: resource_id (u32), padding (u32).
     * Destroys a host resource.  The driver must have already detached all
     * backing memory (RESOURCE_DETACH_BACKING) before calling this, or the
     * host may return VIRTIO_GPU_RESP_ERR_UNSPEC. */

#define VGPU_CMD_SET_SCANOUT            0x0103
    /* Payload: rect (x/y/w/h), scanout_id, resource_id (all u32).
     * Attaches a resource to a scanout and defines the visible rectangle.
     * resource_id 0 disables the scanout. */

#define VGPU_CMD_RESOURCE_FLUSH         0x0104
    /* Payload: rect (x/y/w/h), resource_id, padding (all u32).
     * Asks the host compositor to repaint the named rectangle of the resource
     * on all scanouts that show it.  Must be called after TRANSFER_TO_HOST_2D
     * to make the new pixels visible. */

#define VGPU_CMD_TRANSFER_TO_HOST_2D    0x0105
    /* Payload: rect (x/y/w/h), offset (u64), resource_id, padding (u32).
     * Copies data from the backing pages into the host's resource buffer.
     * @offset is the byte offset within the backing store of the top-left
     * pixel of the rectangle, i.e. y * pitch + x * bytes_per_pixel. */

#define VGPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
    /* Payload: resource_id, nr_entries (u32),
     *          then nr_entries × virtio_gpu_mem_entry (addr u64, len u32, pad u32).
     * Registers guest physical pages as the backing store for a resource.
     * Pages must be physically contiguous within each entry; entries need not
     * be contiguous with each other. */

#define VGPU_CMD_RESOURCE_DETACH_BACKING 0x0107
    /* Payload: resource_id (u32), padding (u32).
     * Unregisters the backing pages.  Call before RESOURCE_UNREF. */

#define VGPU_CMD_GET_CAPSET_INFO        0x0108  /* 3D capability set query      */
#define VGPU_CMD_GET_CAPSET             0x0109  /* 3D capability data fetch     */

#define VGPU_CMD_GET_EDID               0x010A
    /* Requires VGPU_F_EDID feature.
     * Payload: scanout_id (u32), padding (u32).
     * Response: virtio_gpu_resp_edid — size (u32), pad (u32), edid[1024]. */

/* 3D (virgl) commands (controlq) */
#define VGPU_CMD_CTX_CREATE             0x0200
#define VGPU_CMD_CTX_DESTROY            0x0201
#define VGPU_CMD_CTX_ATTACH_RESOURCE    0x0202
#define VGPU_CMD_CTX_DETACH_RESOURCE    0x0203
#define VGPU_CMD_RESOURCE_CREATE_3D     0x0204
#define VGPU_CMD_TRANSFER_TO_HOST_3D    0x0205
#define VGPU_CMD_TRANSFER_FROM_HOST_3D  0x0206
#define VGPU_CMD_SUBMIT_3D              0x0207

/* Cursor queue commands (cursorq) */
#define VGPU_CMD_UPDATE_CURSOR          0x0300
    /* Payload: cursor_pos (scanout_id, x, y, padding), resource_id (u32),
     *          hot_x, hot_y, padding (u32).
     * Uploads a new cursor image (64×64 BGRA) and positions it.
     * resource_id 0 hides the cursor. */

#define VGPU_CMD_MOVE_CURSOR            0x0301
    /* Payload: cursor_pos only (scanout_id, x, y, padding — all u32).
     * Moves the cursor hotspot without changing the image.  Lowest latency
     * path; should be the only command on the cursor queue during normal use. */

/* ── Response codes ───────────────────────────────────────────────────────── */
#define VGPU_RESP_OK_NODATA             0x1100  /* Command succeeded, no payload */
#define VGPU_RESP_OK_DISPLAY_INFO       0x1101  /* GET_DISPLAY_INFO reply        */
#define VGPU_RESP_OK_CAPSET_INFO        0x1102
#define VGPU_RESP_OK_CAPSET             0x1103
#define VGPU_RESP_OK_EDID               0x1104  /* GET_EDID reply                */

#define VGPU_RESP_ERR_UNSPEC            0x1200  /* Unspecified error             */
#define VGPU_RESP_ERR_OUT_OF_MEMORY     0x1201
#define VGPU_RESP_ERR_INVALID_SCANOUT_ID   0x1202
#define VGPU_RESP_ERR_INVALID_RESOURCE_ID  0x1203
#define VGPU_RESP_ERR_INVALID_CONTEXT_ID   0x1204
#define VGPU_RESP_ERR_INVALID_PARAMETER    0x1205

/* ── Pixel formats (virtio_gpu_formats enum values) ──────────────────────── */
#define VGPU_FORMAT_B8G8R8A8_UNORM      1       /* 0xAARRGGBB in memory (LE)    */
#define VGPU_FORMAT_B8G8R8X8_UNORM      2       /* Same, alpha ignored          */
#define VGPU_FORMAT_A8R8G8B8_UNORM      3
#define VGPU_FORMAT_X8R8G8B8_UNORM      4
#define VGPU_FORMAT_R8G8B8A8_UNORM      67      /* 0xAABBGGRR in memory (LE)    */
#define VGPU_FORMAT_X8B8G8R8_UNORM      68
#define VGPU_FORMAT_A8B8G8R8_UNORM      121
#define VGPU_FORMAT_R8G8B8X8_UNORM      134

/* ── Scanout limits ───────────────────────────────────────────────────────── */
#define VGPU_MAX_SCANOUTS               16      /* Spec maximum                  */
#define VGPU_CURSOR_W                   64      /* Hardware cursor width (fixed) */
#define VGPU_CURSOR_H                   64      /* Hardware cursor height (fixed)*/
#define VGPU_EDID_MAX_SIZE              1024    /* Maximum EDID blob size        */

/* ── Control header flags ─────────────────────────────────────────────────── */
#define VGPU_FLAG_FENCE                 (1u << 0)
    /* Set in virtio_gpu_ctrl_hdr.flags to request a fence.  The device will
     * signal the fence (identified by fence_id) when the command completes.
     * Not used by the 2D driver, which polls the used ring instead. */

/* ── Resource-ID conventions (AzamiOS-specific, not in the spec) ─────────── */
#define VGPU_RESOURCE_ID_INVALID        0       /* 0 is never a valid resource   */
#define VGPU_RESOURCE_ID_SCANOUT0       1       /* Scanout 0 framebuffer         */
#define VGPU_RESOURCE_ID_CURSOR         0xC0    /* Shared hardware cursor shape  */
#define VGPU_RESOURCE_ID_EXTRA_BASE     100     /* Scanout 1..N start here       */


/* ════════════════════════════════════════════════════════════════════════════
 * §4  VMWARE SVGA3D HARDWARE INSTRUCTION SET & ARCHITECTURE
 *
 * SVGA3D extends the VMware SVGA II FIFO with a complete hardware 3D pipeline.
 * Commands begin at opcode 1000 and are submitted through the same BAR2 FIFO ring:
 *
 *   1. Define guest surfaces (textures, render targets, depth buffers) with
 *      SVGA_3D_CMD_SURFACE_DEFINE.
 *   2. Create a 3D context with SVGA_3D_CMD_CONTEXT_DEFINE.
 *   3. Bind render targets (SVGA_3D_CMD_SET_RENDER_TARGET), setup viewport
 *      (SVGA_3D_CMD_SET_VIEWPORT), and clear (SVGA_3D_CMD_CLEAR).
 *   4. Set transformation matrices (SVGA_3D_CMD_SET_TRANSFORM), lighting,
 *      materials, and issue draw calls (SVGA_3D_CMD_DRAW_PRIMITIVES).
 *   5. Emit SVGA_REG_SYNC to execute the 3D pipeline on the host GPU.
 * ════════════════════════════════════════════════════════════════════════════ */

#define SVGA_3D_CMD_BASE                        1000
#define SVGA_3D_CMD_SURFACE_DEFINE              1001
#define SVGA_3D_CMD_SURFACE_DESTROY             1002
#define SVGA_3D_CMD_SURFACE_COPY                1003
#define SVGA_3D_CMD_SURFACE_DMA                 1004
#define SVGA_3D_CMD_CONTEXT_DEFINE              1005
#define SVGA_3D_CMD_CONTEXT_DESTROY             1006
#define SVGA_3D_CMD_SET_TRANSFORM               1007
#define SVGA_3D_CMD_SET_RENDER_TARGET           1008
#define SVGA_3D_CMD_SET_RENDER_STATE            1009
#define SVGA_3D_CMD_SET_LIGHT_DATA              1010
#define SVGA_3D_CMD_SET_LIGHT_ENABLED           1011
#define SVGA_3D_CMD_SET_MATERIAL                1012
#define SVGA_3D_CMD_SET_VIEWPORT                1013
#define SVGA_3D_CMD_SET_Z_RANGE                 1014
#define SVGA_3D_CMD_CLEAR                       1015
#define SVGA_3D_CMD_DRAW_PRIMITIVES             1016
#define SVGA_3D_CMD_SET_SCISSOR_RECT            1017
#define SVGA_3D_CMD_SHADER_DEFINE               1018
#define SVGA_3D_CMD_SHADER_DESTROY              1019
#define SVGA_3D_CMD_SET_SHADER                  1020
#define SVGA_3D_CMD_SET_SHADER_CONST            1021

/* SVGA3D Surface Formats */
#define SVGA3D_FORMAT_INVALID                   0
#define SVGA3D_X8R8G8B8                         1
#define SVGA3D_A8R8G8B8                         2
#define SVGA3D_R5G6B5                           3
#define SVGA3D_Z_D16                            4
#define SVGA3D_Z_D24S8                          5
#define SVGA3D_Z_D24X8                          6

/* SVGA3D Primitive Topologies */
#define SVGA3D_PRIMITIVE_TRIANGLELIST           1
#define SVGA3D_PRIMITIVE_TRIANGLESTRIP          2
#define SVGA3D_PRIMITIVE_TRIANGLEFAN            3
#define SVGA3D_PRIMITIVE_LINELIST               4
#define SVGA3D_PRIMITIVE_LINESTRIP              5
#define SVGA3D_PRIMITIVE_POINTLIST              6

/* SVGA3D Shader Types */
#define SVGA3D_SHADERTYPE_VS                    1   /* Vertex Shader   */
#define SVGA3D_SHADERTYPE_PS                    2   /* Pixel Shader    */
#define SVGA3D_SHADERTYPE_GS                    3   /* Geometry Shader */


/* ════════════════════════════════════════════════════════════════════════════
 * §5  VIRTIO-GPU VIRGL 3D HARDWARE INSTRUCTION SET & TGSI BYTECODE ISA
 *
 * VirtIO-GPU 3D (virglrenderer) passes command control streams (CCMD) over the
 * controlq virtqueue. Packets use a unified 32-bit header:
 *
 *   [31:16] length in DWORDs
 *   [15:8]  object type
 *   [7:0]   command opcode (VIRGL_CCMD_*)
 * ════════════════════════════════════════════════════════════════════════════ */

#define VIRGL_CMD_HDR(cmd, obj, len) \
    (((u32)(len) << 16) | (((u32)(obj) & 0xFF) << 8) | ((u32)(cmd) & 0xFF))

/* Virgl Control Command (CCMD) Opcodes */
#define VIRGL_CCMD_NOP                          0
#define VIRGL_CCMD_CREATE_OBJECT                1
#define VIRGL_CCMD_BIND_OBJECT                  2
#define VIRGL_CCMD_DESTROY_OBJECT               3
#define VIRGL_CCMD_SET_VIEWPORT_STATE           4
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE        5
#define VIRGL_CCMD_SET_VERTEX_BUFFERS           6
#define VIRGL_CCMD_CLEAR                        7
#define VIRGL_CCMD_DRAW_VBO                     8
#define VIRGL_CCMD_RESOURCE_INLINE_WRITE        9
#define VIRGL_CCMD_SET_SAMPLER_VIEWS            10
#define VIRGL_CCMD_SET_INDEX_BUFFER             11
#define VIRGL_CCMD_SET_CONSTANT_BUFFER          12
#define VIRGL_CCMD_SET_STENCIL_REF              13
#define VIRGL_CCMD_SET_BLEND_COLOR              14
#define VIRGL_CCMD_SET_SCISSOR_STATE            15
#define VIRGL_CCMD_BLIT                         16
#define VIRGL_CCMD_RESOURCE_COPY_REGION         17
#define VIRGL_CCMD_BIND_SAMPLER_STATES          18

/* Virgl Object Types */
#define VIRGL_OBJECT_BLEND                      1
#define VIRGL_OBJECT_RASTERIZER                 2
#define VIRGL_OBJECT_DSA                        3   /* Depth-Stencil-Alpha */
#define VIRGL_OBJECT_SHADER                     4
#define VIRGL_OBJECT_VERTEX_ELEMENTS            5
#define VIRGL_OBJECT_SAMPLER_VIEW               6
#define VIRGL_OBJECT_SAMPLER_STATE              7
#define VIRGL_OBJECT_SURFACE                    8
#define VIRGL_OBJECT_QUERY                      9
#define VIRGL_OBJECT_STREAMOUT_TARGET           10

/* Gallium Pipe Formats (Virgl Surface Formats) */
#define PIPE_FORMAT_B8G8R8A8_UNORM              1
#define PIPE_FORMAT_B8G8R8X8_UNORM              2
#define PIPE_FORMAT_R8G8B8A8_UNORM              67
#define PIPE_FORMAT_Z24X8_UNORM                 16
#define PIPE_FORMAT_Z24_UNORM_S8_UINT           17

/* TGSI (Tungsten Graphics Shader Infrastructure) Bytecode Opcodes */
#define TGSI_OPCODE_ARL                         0
#define TGSI_OPCODE_MOV                         1
#define TGSI_OPCODE_LIT                         2
#define TGSI_OPCODE_RCP                         3
#define TGSI_OPCODE_RSQ                         4
#define TGSI_OPCODE_EXP                         5
#define TGSI_OPCODE_LOG                         6
#define TGSI_OPCODE_MUL                         7
#define TGSI_OPCODE_ADD                         8
#define TGSI_OPCODE_DP3                         9
#define TGSI_OPCODE_DP4                         10
#define TGSI_OPCODE_DST                         11
#define TGSI_OPCODE_MIN                         12
#define TGSI_OPCODE_MAX                         13
#define TGSI_OPCODE_SLT                         14
#define TGSI_OPCODE_SGE                         15
#define TGSI_OPCODE_MAD                         16
#define TGSI_OPCODE_SUB                         17
#define TGSI_OPCODE_TEX                         20
#define TGSI_OPCODE_RET                         33
#define TGSI_OPCODE_END                         34


/* ════════════════════════════════════════════════════════════════════════════
 * §6  INTEL GEN / IRIS GRAPHICS COMMAND STREAMER & EU ISA
 *
 * Intel integrated GPUs (Gen 7/8/9/11/12/Xe) execute commands via hardware
 * ring buffers and the Command Streamer (CS).
 * Command streams are divided into:
 *   - Memory Interface (MI) Commands: flow control, synchronization, batch execution
 *   - 3D Pipeline Commands: state configuration and 3DPRIMITIVE draw submission
 *   - Execution Unit (EU) SIMD Instructions: microcode running on EU ALUs
 * ════════════════════════════════════════════════════════════════════════════ */

/* MI Command Instructions */
#define INTEL_MI_CMD(opcode)                    ((0x0u << 29) | ((opcode) << 23))
#define MI_NOOP                                 INTEL_MI_CMD(0x00)
#define MI_USER_INTERRUPT                       INTEL_MI_CMD(0x02)
#define MI_WAIT_FOR_EVENT                       INTEL_MI_CMD(0x03)
#define MI_FLUSH                                INTEL_MI_CMD(0x04)
#define MI_ARB_CHECK                            INTEL_MI_CMD(0x05)
#define MI_REPORT_HEAD                          INTEL_MI_CMD(0x07)
#define MI_STORE_DATA_IMM                       INTEL_MI_CMD(0x20)
#define MI_STORE_DATA_INDEX                     INTEL_MI_CMD(0x21)
#define MI_LOAD_REGISTER_IMM                    INTEL_MI_CMD(0x22)
#define MI_STORE_REGISTER_MEM                   INTEL_MI_CMD(0x24)
#define MI_BATCH_BUFFER_START                   INTEL_MI_CMD(0x31)
#define MI_BATCH_BUFFER_END                     INTEL_MI_CMD(0x0A)
#define MI_SEMAPHORE_SIGNAL                     INTEL_MI_CMD(0x1B)
#define MI_SEMAPHORE_WAIT                       INTEL_MI_CMD(0x1C)

/* 3D Pipeline State & Primitive Opcodes */
#define GFX_CMD_3D_STATE_BASE_ADDRESS           0x7901
#define GFX_CMD_3D_STATE_BINDING_TABLE_POINTERS_VS 0x7826
#define GFX_CMD_3D_STATE_BINDING_TABLE_POINTERS_PS 0x782A
#define GFX_CMD_3D_STATE_VERTEX_BUFFERS         0x7808
#define GFX_CMD_3D_STATE_VERTEX_ELEMENTS        0x7809
#define GFX_CMD_3D_STATE_VS                     0x7810
#define GFX_CMD_3D_STATE_PS                     0x7820
#define GFX_CMD_3D_STATE_VIEWPORT_STATE_POINTERS_CC 0x7823
#define GFX_CMD_3D_STATE_CC_STATE_POINTERS      0x780E
#define GFX_CMD_3D_STATE_DEPTH_BUFFER           0x7905
#define GFX_CMD_3D_STATE_CLEAR_PARAMS           0x7910
#define GFX_CMD_3DPRIMITIVE                     0x7B00

/* Intel Execution Unit (EU) SIMD Register Architecture:
 *   - GRF (General Register File): 128 registers (r0..r127), 256 bits (32 bytes) each.
 *   - ARF (Architecture Register File):
 *       null (null register)
 *       a0   (address register for indexed addressing)
 *       acc0..acc1 (accumulators for multiply-accumulate)
 *       f0..f1     (flag registers for conditional execution)
 *       cr0        (control register) */


/* ════════════════════════════════════════════════════════════════════════════
 * §7  AMD RADEON / RDNA / GCN COMMAND PROCESSOR (CP) HARDWARE ISA
 *
 * AMD GPUs process graphics commands through the Command Processor (CP) using
 * Packet 3 packets. A Packet 3 has a 32-bit header followed by N DWORD payload:
 *
 *   [31:30] Packet Type = 3 (0b11)
 *   [29:16] Count (number of DWORDs in payload - 1)
 *   [15:8]  Opcode (PACKET3_*)
 *   [7:0]   Reserved / predicate
 * ════════════════════════════════════════════════════════════════════════════ */

#define AMD_PACKET3_HDR(opcode, count) \
    ((3u << 30) | (((u32)(count) & 0x3FFFu) << 16) | (((u32)(opcode) & 0xFFu) << 8))

/* Packet 3 Opcodes */
#define PACKET3_NOP                             0x10
#define PACKET3_SET_BASE                        0x11
#define PACKET3_CLEAR_STATE                     0x12
#define PACKET3_INDEX_BUFFER_SIZE               0x13
#define PACKET3_DISPATCH_DIRECT                 0x15
#define PACKET3_DISPATCH_INDIRECT               0x16
#define PACKET3_DRAW_INDEX_AUTO                 0x2D
#define PACKET3_DRAW_INDEX_2                    0x27
#define PACKET3_INDEX_TYPE                      0x2A
#define PACKET3_NUM_INSTANCES                   0x2F
#define PACKET3_SET_CONTEXT_REG                 0x69
#define PACKET3_SET_SH_REG                      0x76
#define PACKET3_WAIT_REG_MEM                    0x3C
#define PACKET3_RELEASE_MEM                     0x49

/* AMD Wavefront Execution Model:
 *   - Wave32 / Wave64 SIMD execution threads lockstep per Compute Unit (CU).
 *   - VGPR (Vector General Purpose Register) file per SIMD lane.
 *   - SGPR (Scalar General Purpose Register) shared across the entire wavefront.
 *   - Dual execution pipelines: SALU (Scalar ALU) and VALU (Vector ALU). */


/* ════════════════════════════════════════════════════════════════════════════
 * §8  COMMON PIXEL FORMAT ALIASES
 *
 * Standard aliases that resolve to each backend's native constant.
 * ════════════════════════════════════════════════════════════════════════════ */

/* 32-bit BGRA — native for BGA/SVGA and the virtio-gpu default.
 * Wire format (little-endian): byte0=B, byte1=G, byte2=R, byte3=A. */
#define GPU_FORMAT_BGRA8888             VGPU_FORMAT_B8G8R8A8_UNORM

/* 32-bit BGRX — same as BGRA8888 with alpha forced to 0xFF by convention. */
#define GPU_FORMAT_BGRX8888             VGPU_FORMAT_B8G8R8X8_UNORM

/* DRM FourCC equivalents (from include/azami/drm.h):
 *   DRM_FORMAT_ARGB8888  0x34325241  — what Mesa calls "BGRA on the wire"
 *   DRM_FORMAT_XRGB8888  0x34325258  — same without alpha
 * These match VGPU_FORMAT_B8G8R8A8_UNORM and VGPU_FORMAT_B8G8R8X8_UNORM
 * exactly for the x86 little-endian host/guest pair we target.           */


/* ════════════════════════════════════════════════════════════════════════════
 * §9  WRITE-COMBINING I/O HELPERS  (reference; implementations in hwaccel.h)
 *
 * These are *not* declared here — include arch/x86_64/cpu/hwaccel.h for the
 * actual inline implementations.  This section documents which primitive to
 * use for each GPU operation category.
 *
 * Pixel fill (solid colour, whole region):
 *   hw_fill_vram(dst, color, pixel_count)   — ERMS rep-stosq or MOVNTI loop
 *
 * Pixel copy (shadow buffer → VRAM or resource backing):
 *   hw_copy_to_vram(dst, src, byte_count)   — ERMS rep-movsb or MOVNTI loop
 *
 * Targeted blit (one damage rectangle from a GEM object):
 *   drm_gem_blit_rect(obj, dst_virt, dst_pitch, clip, bpp)  — calls above
 *
 * Non-temporal 32/64-bit store (for hand-rolled GPU control struct writes):
 *   hw_nt_store32(ptr, val)    — MOVNTI dword, no cache pollution
 *   hw_nt_store64(ptr, val)    — MOVNTI qword, no cache pollution
 *
 * Software prefetch (bringing a shadow-buffer row into L1 ahead of the blit):
 *   hw_prefetch_read(ptr)      — PREFETCHNTA (minimal cache pollution)
 *   hw_prefetch_write(ptr)     — PREFETCHT0  (fill L1 for write)
 *
 * Store fence (required before any device doorbell / register write):
 *   hw_sfence()                — SFENCE — all stores visible before the bell
 * ════════════════════════════════════════════════════════════════════════════ */
