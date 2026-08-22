/* ============================================================================
 * AzamiOS — Direct Rendering Manager (DRM/KMS) Subsystem (/dev/dri/card0)
 * File: drivers/video/drm.c
 *
 * Implements Linux-compatible Direct Rendering Manager (DRM/KMS) character
 * devices (/dev/dri/card0 and /dev/dri/renderD128) with:
 *  - Modesetting resource discovery (CRTCs, Connectors, Encoders, Modes)
 *  - Dumb buffer allocation and page-aligned mmap mapping
 *  - Mode setting & page flipping ioctl dispatch
 * ============================================================================ */

#include "drm.h"
#include "../../fs/vfs.h"
#include "../../kernel/lib/string.h"
#include "../../kernel/uaccess.h"
#include "../../kernel/mm/pmm.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../kernel/sched/sched.h"
#include "../misc/bga.h"
#include <azami/debug.h>

#define DRM_IOCTL_BASE                  'd'
#define DRM_IOCTL_VERSION               0xC0406400
#define DRM_IOCTL_GET_CAP               0xC010640C
#define DRM_IOCTL_SET_CLIENT_CAP        0x4010640D
#define DRM_IOCTL_MODE_GETRESOURCES     0xC04064A0
#define DRM_IOCTL_MODE_GETCRTC          0xC06864A1
#define DRM_IOCTL_MODE_SETCRTC          0xC06864A2
#define DRM_IOCTL_MODE_GETCONNECTOR     0xC05064A7
#define DRM_IOCTL_MODE_ADDFB            0xC01C64AE
#define DRM_IOCTL_MODE_RMFB             0xC00464AF
#define DRM_IOCTL_MODE_PAGE_FLIP        0xC01864B0
#define DRM_IOCTL_MODE_CREATE_DUMB      0xC02064B2
#define DRM_IOCTL_MODE_MAP_DUMB         0xC01064B3
#define DRM_IOCTL_MODE_DESTROY_DUMB     0xC00464B4

#define DRM_CAP_DUMB_BUFFER             0x1
#define DRM_CAP_PRIME                   0x5

#define MAX_DUMB_BUFFERS 32

typedef struct {
    u32         handle;
    u32         width;
    u32         height;
    u32         bpp;
    u32         pitch;
    size_t      size;
    phys_addr_t *pages;
    size_t      page_count;
    u64         mmap_offset;
} drm_dumb_buffer_t;

typedef struct {
    drm_dumb_buffer_t buffers[MAX_DUMB_BUFFERS];
    u32               next_handle;
    u64               next_mmap_offset;
    spinlock_t        lock;
} drm_device_state_t;

static drm_device_state_t g_drm_state = {
    .next_handle      = 1,
    .next_mmap_offset = 0x100000000ULL,
    .lock             = SPINLOCK_INIT,
};

extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);

/* ── DRM IOCTL Structs ────────────────────────────────────────────────────── */
struct drm_version_uapi {
    int    version_major;
    int    version_minor;
    int    version_patchlevel;
    size_t name_len;
    char  *name;
    size_t date_len;
    char  *date;
    size_t desc_len;
    char  *desc;
};

struct drm_get_cap_uapi {
    u64 capability;
    u64 value;
};

struct drm_mode_modeinfo_uapi {
    u32  clock;
    u16  hdisplay;
    u16  hsync_start;
    u16  hsync_end;
    u16  htotal;
    u16  hskew;
    u16  vdisplay;
    u16  vsync_start;
    u16  vsync_end;
    u16  vtotal;
    u16  vscan;
    u32  vrefresh;
    u32  flags;
    u32  type;
    char name[32];
};

struct drm_mode_card_res_uapi {
    u64 fb_id_ptr;
    u64 crtc_id_ptr;
    u64 connector_id_ptr;
    u64 encoder_id_ptr;
    u32 count_fbs;
    u32 count_crtcs;
    u32 count_connectors;
    u32 count_encoders;
    u32 min_width;
    u32 max_width;
    u32 min_height;
    u32 max_height;
};

struct drm_mode_crtc_uapi {
    u64 set_connectors_ptr;
    u32 count_connectors;
    u32 crtc_id;
    u32 fb_id;
    u32 x;
    u32 y;
    u32 gamma_size;
    u32 mode_valid;
    struct drm_mode_modeinfo_uapi mode;
};

struct drm_mode_get_connector_uapi {
    u64 encoders_ptr;
    u64 modes_ptr;
    u64 props_ptr;
    u64 prop_values_ptr;
    u32 count_modes;
    u32 count_props;
    u32 count_encoders;
    u32 encoder_id;
    u32 connector_id;
    u32 connector_type;
    u32 connector_type_id;
    u32 connection;
    u32 mm_width;
    u32 mm_height;
    u32 subpixel;
    u32 pad;
};

struct drm_mode_create_dumb_uapi {
    u32 height;
    u32 width;
    u32 bpp;
    u32 flags;
    u32 handle;
    u32 pitch;
    u64 size;
};

struct drm_mode_map_dumb_uapi {
    u32 handle;
    u32 pad;
    u64 offset;
};

struct drm_mode_destroy_dumb_uapi {
    u32 handle;
};

struct drm_mode_fb_cmd_uapi {
    u32 fb_id;
    u32 width;
    u32 height;
    u32 pitch;
    u32 bpp;
    u32 depth;
    u32 handle;
};

struct drm_mode_crtc_page_flip_uapi {
    u32 crtc_id;
    u32 fb_id;
    u32 flags;
    u32 reserved;
    u64 user_data;
};

/* ── File Operations ──────────────────────────────────────────────────────── */

static s64 drm_open(struct inode *inode, struct file *filp)
{
    (void)inode;
    filp->private_data = &g_drm_state;
    return 0;
}

static s64 drm_release(struct inode *inode, struct file *filp)
{
    (void)inode;
    (void)filp;
    return 0;
}

static s64 drm_ioctl(struct file *filp, u32 cmd, u64 arg)
{
    (void)filp;
    if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EFAULT;

    switch (cmd) {
        case DRM_IOCTL_VERSION: {
            struct drm_version_uapi ver;
            if (copy_from_user(&ver, (void *)(uintptr_t)arg, sizeof(ver)) != 0) return -(s64)EFAULT;

            ver.version_major = 1;
            ver.version_minor = 0;
            ver.version_patchlevel = 0;

            const char *name = "azami-drm";
            const char *date = "20260822";
            const char *desc = "AzamiOS DRM/KMS KMS Direct Rendering Driver";

            if (ver.name && ver.name_len > 0) {
                size_t l = strlen(name);
                if (l >= ver.name_len) l = ver.name_len - 1;
                copy_to_user(ver.name, name, l);
            }
            if (ver.date && ver.date_len > 0) {
                size_t l = strlen(date);
                if (l >= ver.date_len) l = ver.date_len - 1;
                copy_to_user(ver.date, date, l);
            }
            if (ver.desc && ver.desc_len > 0) {
                size_t l = strlen(desc);
                if (l >= ver.desc_len) l = ver.desc_len - 1;
                copy_to_user(ver.desc, desc, l);
            }

            if (copy_to_user((void *)(uintptr_t)arg, &ver, sizeof(ver)) != 0) return -(s64)EFAULT;
            return 0;
        }

        case DRM_IOCTL_GET_CAP: {
            struct drm_get_cap_uapi cap;
            if (copy_from_user(&cap, (void *)(uintptr_t)arg, sizeof(cap)) != 0) return -(s64)EFAULT;

            if (cap.capability == DRM_CAP_DUMB_BUFFER) {
                cap.value = 1;
            } else if (cap.capability == DRM_CAP_PRIME) {
                cap.value = 1;
            } else {
                cap.value = 0;
            }

            if (copy_to_user((void *)(uintptr_t)arg, &cap, sizeof(cap)) != 0) return -(s64)EFAULT;
            return 0;
        }

        case DRM_IOCTL_MODE_GETRESOURCES: {
            struct drm_mode_card_res_uapi res;
            if (copy_from_user(&res, (void *)(uintptr_t)arg, sizeof(res)) != 0) return -(s64)EFAULT;

            res.count_fbs        = 1;
            res.count_crtcs      = 1;
            res.count_connectors = 1;
            res.count_encoders   = 1;
            res.min_width        = 640;
            res.max_width        = 3840;
            res.min_height       = 480;
            res.max_height       = 2160;

            u32 crtc_id = 1;
            u32 conn_id = 1;
            u32 enc_id  = 1;
            u32 fb_id   = 1;

            if (res.crtc_id_ptr && res.count_crtcs > 0) {
                copy_to_user((void *)(uintptr_t)res.crtc_id_ptr, &crtc_id, sizeof(u32));
            }
            if (res.connector_id_ptr && res.count_connectors > 0) {
                copy_to_user((void *)(uintptr_t)res.connector_id_ptr, &conn_id, sizeof(u32));
            }
            if (res.encoder_id_ptr && res.count_encoders > 0) {
                copy_to_user((void *)(uintptr_t)res.encoder_id_ptr, &enc_id, sizeof(u32));
            }
            if (res.fb_id_ptr && res.count_fbs > 0) {
                copy_to_user((void *)(uintptr_t)res.fb_id_ptr, &fb_id, sizeof(u32));
            }

            if (copy_to_user((void *)(uintptr_t)arg, &res, sizeof(res)) != 0) return -(s64)EFAULT;
            return 0;
        }

        case DRM_IOCTL_MODE_GETCRTC: {
            struct drm_mode_crtc_uapi crtc;
            if (copy_from_user(&crtc, (void *)(uintptr_t)arg, sizeof(crtc)) != 0) return -(s64)EFAULT;

            u32 w = bga_get_width() ? bga_get_width() : 1280;
            u32 h = bga_get_height() ? bga_get_height() : 800;

            crtc.crtc_id          = 1;
            crtc.fb_id            = 1;
            crtc.x                = 0;
            crtc.y                = 0;
            crtc.gamma_size       = 0;
            crtc.mode_valid       = 1;
            crtc.mode.clock       = 74250;
            crtc.mode.hdisplay    = (u16)w;
            crtc.mode.vdisplay    = (u16)h;
            crtc.mode.vrefresh    = 60;
            strncpy(crtc.mode.name, "1280x800", sizeof(crtc.mode.name) - 1);

            if (copy_to_user((void *)(uintptr_t)arg, &crtc, sizeof(crtc)) != 0) return -(s64)EFAULT;
            return 0;
        }

        case DRM_IOCTL_MODE_GETCONNECTOR: {
            struct drm_mode_get_connector_uapi conn;
            if (copy_from_user(&conn, (void *)(uintptr_t)arg, sizeof(conn)) != 0) return -(s64)EFAULT;

            u32 w = bga_get_width() ? bga_get_width() : 1280;
            u32 h = bga_get_height() ? bga_get_height() : 800;

            conn.connector_id      = 1;
            conn.connector_type    = 11; /* DRM_MODE_CONNECTOR_HDMIA */
            conn.connector_type_id = 1;
            conn.connection        = 1;  /* Connected */
            conn.mm_width          = 330;
            conn.mm_height         = 210;
            conn.subpixel          = 1;  /* Horizontal RGB */
            conn.count_modes       = 1;
            conn.count_encoders    = 1;

            if (conn.encoders_ptr && conn.count_encoders > 0) {
                u32 enc_id = 1;
                copy_to_user((void *)(uintptr_t)conn.encoders_ptr, &enc_id, sizeof(u32));
            }

            if (conn.modes_ptr && conn.count_modes > 0) {
                struct drm_mode_modeinfo_uapi mode;
                __builtin_memset(&mode, 0, sizeof(mode));
                mode.clock    = 74250;
                mode.hdisplay = (u16)w;
                mode.vdisplay = (u16)h;
                mode.vrefresh = 60;
                mode.type     = 0x0A; /* Preferred | Driver */
                strncpy(mode.name, "1280x800", sizeof(mode.name) - 1);
                copy_to_user((void *)(uintptr_t)conn.modes_ptr, &mode, sizeof(mode));
            }

            if (copy_to_user((void *)(uintptr_t)arg, &conn, sizeof(conn)) != 0) return -(s64)EFAULT;
            return 0;
        }

        case DRM_IOCTL_MODE_CREATE_DUMB: {
            struct drm_mode_create_dumb_uapi dumb;
            if (copy_from_user(&dumb, (void *)(uintptr_t)arg, sizeof(dumb)) != 0) return -(s64)EFAULT;

            u32 bpp = dumb.bpp ? dumb.bpp : 32;
            u32 pitch = ALIGN_UP(dumb.width * ((bpp + 7) / 8), 64);
            size_t size = (size_t)pitch * dumb.height;
            size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

            spinlock_lock(&g_drm_state.lock);
            int slot = -1;
            for (int i = 0; i < MAX_DUMB_BUFFERS; i++) {
                if (g_drm_state.buffers[i].handle == 0) {
                    slot = i;
                    break;
                }
            }
            if (slot == -1) {
                spinlock_unlock(&g_drm_state.lock);
                return -(s64)ENOMEM;
            }

            phys_addr_t *allocated_pages = (phys_addr_t *)kzalloc(pages * sizeof(phys_addr_t));
            if (!allocated_pages) {
                spinlock_unlock(&g_drm_state.lock);
                return -(s64)ENOMEM;
            }

            for (size_t p = 0; p < pages; p++) {
                phys_addr_t page = pmm_alloc_page();
                if (!page) {
                    for (size_t k = 0; k < p; k++) pmm_free_page(allocated_pages[k]);
                    kfree(allocated_pages);
                    spinlock_unlock(&g_drm_state.lock);
                    return -(s64)ENOMEM;
                }
                __builtin_memset((void *)PHYS_TO_VIRT(page), 0, PAGE_SIZE);
                allocated_pages[p] = page;
            }

            u32 handle = g_drm_state.next_handle++;
            u64 offset = g_drm_state.next_mmap_offset;
            g_drm_state.next_mmap_offset += ALIGN_UP(size, 0x10000000ULL); /* 256MB step */

            g_drm_state.buffers[slot].handle      = handle;
            g_drm_state.buffers[slot].width       = dumb.width;
            g_drm_state.buffers[slot].height      = dumb.height;
            g_drm_state.buffers[slot].bpp         = bpp;
            g_drm_state.buffers[slot].pitch       = pitch;
            g_drm_state.buffers[slot].size        = size;
            g_drm_state.buffers[slot].pages       = allocated_pages;
            g_drm_state.buffers[slot].page_count  = pages;
            g_drm_state.buffers[slot].mmap_offset = offset;
            spinlock_unlock(&g_drm_state.lock);

            dumb.handle = handle;
            dumb.pitch  = pitch;
            dumb.size   = size;

            if (copy_to_user((void *)(uintptr_t)arg, &dumb, sizeof(dumb)) != 0) return -(s64)EFAULT;
            return 0;
        }

        case DRM_IOCTL_MODE_MAP_DUMB: {
            struct drm_mode_map_dumb_uapi map;
            if (copy_from_user(&map, (void *)(uintptr_t)arg, sizeof(map)) != 0) return -(s64)EFAULT;

            spinlock_lock(&g_drm_state.lock);
            u64 offset = 0;
            for (int i = 0; i < MAX_DUMB_BUFFERS; i++) {
                if (g_drm_state.buffers[i].handle == map.handle) {
                    offset = g_drm_state.buffers[i].mmap_offset;
                    break;
                }
            }
            spinlock_unlock(&g_drm_state.lock);

            if (!offset) return -(s64)EINVAL;
            map.offset = offset;

            if (copy_to_user((void *)(uintptr_t)arg, &map, sizeof(map)) != 0) return -(s64)EFAULT;
            return 0;
        }

        case DRM_IOCTL_MODE_DESTROY_DUMB: {
            struct drm_mode_destroy_dumb_uapi dest;
            if (copy_from_user(&dest, (void *)(uintptr_t)arg, sizeof(dest)) != 0) return -(s64)EFAULT;

            spinlock_lock(&g_drm_state.lock);
            for (int i = 0; i < MAX_DUMB_BUFFERS; i++) {
                if (g_drm_state.buffers[i].handle == dest.handle) {
                    for (size_t p = 0; p < g_drm_state.buffers[i].page_count; p++) {
                        pmm_free_page(g_drm_state.buffers[i].pages[p]);
                    }
                    kfree(g_drm_state.buffers[i].pages);
                    __builtin_memset(&g_drm_state.buffers[i], 0, sizeof(drm_dumb_buffer_t));
                    break;
                }
            }
            spinlock_unlock(&g_drm_state.lock);
            return 0;
        }

        case DRM_IOCTL_MODE_ADDFB:
            return 0;

        case DRM_IOCTL_MODE_PAGE_FLIP: {
            bga_flip_buffer(0);
            return 0;
        }

        default:
            return -(s64)EINVAL;
    }
}

static s64 drm_mmap(struct file *filp, virt_addr_t vaddr, size_t len, u32 prot, u32 flags, u64 offset)
{
    (void)filp;
    (void)prot;
    (void)flags;

    process_t *proc = sched_current_process();
    if (!proc || !proc->pml4_phys) return -(s64)EPERM;

    spinlock_lock(&g_drm_state.lock);
    drm_dumb_buffer_t *target = NULL;
    for (int i = 0; i < MAX_DUMB_BUFFERS; i++) {
        if (g_drm_state.buffers[i].handle != 0 && g_drm_state.buffers[i].mmap_offset == offset) {
            target = &g_drm_state.buffers[i];
            break;
        }
    }

    if (!target) {
        spinlock_unlock(&g_drm_state.lock);
        return -(s64)EINVAL;
    }

    size_t map_pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    if (map_pages > target->page_count) map_pages = target->page_count;

    u64 vmm_flags = VMM_F_PRESENT | VMM_F_USER | VMM_F_WRITE | VMM_F_NX | VMM_F_PWT;
    for (size_t i = 0; i < map_pages; i++) {
        vmm_map(proc->pml4_phys, vaddr + i * PAGE_SIZE, target->pages[i], vmm_flags);
    }
    spinlock_unlock(&g_drm_state.lock);

    return 0;
}

static file_operations_t drm_fops = {
    .open    = drm_open,
    .release = drm_release,
    .ioctl   = drm_ioctl,
    .mmap    = drm_mmap,
};

void drm_init(void)
{
    devfs_register_device("card0", &drm_fops, &g_drm_state);
    devfs_register_device("dri/card0", &drm_fops, &g_drm_state);
    devfs_register_device("dri/renderD128", &drm_fops, &g_drm_state);
    pr_debug("[DRM] Linux Direct Rendering Manager (DRM/KMS) /dev/dri/card0 and /dev/card0 registered\n");
}
