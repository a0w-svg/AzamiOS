/*
 * azamiOS win32k - Secure 2D Compositor & Page-Flipped Framebuffer Driver
 * Path: /kernel/azami/drivers/win32k.c
 * Description: Page-flipped framebuffer driver and secure 2D graphical window
 *              compositor with strict bounding-box clipping and Z-order sorting.
 */

#include "../include/azami_core.h"

/* =========================================================================
 * STATIC VRAM AND COMPOSITOR POOLS (Zero Post-Boot Dynamic Allocation)
 * ========================================================================= */

static uint8_t g_fb_vram_page0[AZ_FB_PAGE_SIZE];
static uint8_t g_fb_vram_page1[AZ_FB_PAGE_SIZE];

static az_window_t g_windows[AZ_WIN32K_MAX_WINDOWS];
static az_spinlock_t g_win32k_lock;

/* =========================================================================
 * INITIALIZATION
 * ========================================================================= */

az_status_t az_win32k_init(void) {
    az_spinlock_init(&g_win32k_lock);
    az_spinlock_acquire(&g_win32k_lock);

    for (size_t i = 0U; i < AZ_FB_PAGE_SIZE; i++) {
        g_fb_vram_page0[i] = 0U;
        g_fb_vram_page1[i] = 0U;
    }

    for (size_t i = 0U; i < AZ_WIN32K_MAX_WINDOWS; i++) {
        g_windows[i].window_id = (uint32_t)i;
        g_windows[i].surface_handle = AZ_INVALID_HANDLE;
        g_windows[i].bounds.x = 0;
        g_windows[i].bounds.y = 0;
        g_windows[i].bounds.width = 0U;
        g_windows[i].bounds.height = 0U;
        g_windows[i].clip_rect.x = 0;
        g_windows[i].clip_rect.y = 0;
        g_windows[i].clip_rect.width = 0U;
        g_windows[i].clip_rect.height = 0U;
        g_windows[i].z_order = 0U;
        g_windows[i].is_visible = false;
        g_windows[i].background_color = 0x00000000U;
        g_windows[i].is_allocated = false;
    }

    az_spinlock_release(&g_win32k_lock);
    return AZ_STATUS_SUCCESS;
}

/* =========================================================================
 * SURFACE MANAGEMENT & PAGE FLIPPING
 * ========================================================================= */

az_status_t az_win32k_surface_create(uint32_t width, uint32_t height, az_object_t** out_surface) {
    if (out_surface == NULL || width == 0U || height == 0U || width > AZ_FB_MAX_WIDTH || height > AZ_FB_MAX_HEIGHT) {
        return AZ_STATUS_INVALID_PARAMETER;
    }
    *out_surface = NULL;

    az_object_t* surf_obj = NULL;
    az_status_t status = az_object_create(AZ_OBJ_TYPE_SURFACE, "win32k_surface", &surf_obj);
    if (AZ_ERROR(status) || surf_obj == NULL) {
        return status;
    }

    az_spinlock_acquire(&surf_obj->lock);
    az_surface_t* s = &surf_obj->data.surface;
    s->width = width;
    s->height = height;
    s->bpp = AZ_FB_BPP;
    s->pitch = width * (AZ_FB_BPP / 8U);
    s->active_page_index = 0U;
    s->framebuffer_pages[0] = g_fb_vram_page0;
    s->framebuffer_pages[1] = g_fb_vram_page1;
    az_spinlock_release(&surf_obj->lock);

    *out_surface = surf_obj;
    return AZ_STATUS_SUCCESS;
}

az_status_t az_win32k_surface_page_flip(az_object_t* surface_obj) {
    if (surface_obj == NULL || surface_obj->type != AZ_OBJ_TYPE_SURFACE || !surface_obj->is_allocated) {
        return AZ_STATUS_INVALID_PARAMETER;
    }

    az_spinlock_acquire(&surface_obj->lock);
    surface_obj->data.surface.active_page_index ^= 1U;
    az_spinlock_release(&surface_obj->lock);

    return AZ_STATUS_SUCCESS;
}

/* =========================================================================
 * WINDOW CREATION & SECURE BOUNDING BOX CLIPPING
 * ========================================================================= */

az_status_t az_win32k_window_create(az_handle_t surface_handle, const az_rect_t* bounds, uint32_t z_order, uint32_t* out_window_id) {
    if (bounds == NULL || out_window_id == NULL || surface_handle == AZ_INVALID_HANDLE) {
        return AZ_STATUS_INVALID_PARAMETER;
    }
    *out_window_id = 0xFFFFFFFFU;

    if (bounds->width == 0U || bounds->height == 0U || bounds->x < 0 || bounds->y < 0) {
        return AZ_STATUS_INVALID_PARAMETER;
    }
    if (((uint32_t)bounds->x + bounds->width) > AZ_FB_MAX_WIDTH || ((uint32_t)bounds->y + bounds->height) > AZ_FB_MAX_HEIGHT) {
        return AZ_STATUS_INVALID_PARAMETER;
    }

    az_spinlock_acquire(&g_win32k_lock);
    az_window_t* slot = NULL;
    for (size_t i = 0U; i < AZ_WIN32K_MAX_WINDOWS; i++) {
        if (!g_windows[i].is_allocated) {
            slot = &g_windows[i];
            slot->is_allocated = true;
            break;
        }
    }

    if (slot == NULL) {
        az_spinlock_release(&g_win32k_lock);
        return AZ_STATUS_INSUFFICIENT_RESOURCES;
    }

    slot->surface_handle = surface_handle;
    slot->bounds = *bounds;
    slot->clip_rect = *bounds;
    slot->z_order = z_order;
    slot->is_visible = true;
    slot->background_color = 0xFF1E1E1EU;
    *out_window_id = slot->window_id;

    az_spinlock_release(&g_win32k_lock);
    return AZ_STATUS_SUCCESS;
}

az_status_t az_win32k_window_fill_rect_clipped(uint32_t window_id, const az_rect_t* rect, uint32_t color) {
    if (rect == NULL || window_id >= AZ_WIN32K_MAX_WINDOWS) {
        return AZ_STATUS_INVALID_PARAMETER;
    }

    az_spinlock_acquire(&g_win32k_lock);
    az_window_t* win = &g_windows[window_id];
    if (!win->is_allocated || !win->is_visible) {
        az_spinlock_release(&g_win32k_lock);
        return AZ_STATUS_INVALID_HANDLE;
    }

    int32_t clip_x1 = win->clip_rect.x;
    int32_t clip_y1 = win->clip_rect.y;
    int32_t clip_x2 = win->clip_rect.x + (int32_t)win->clip_rect.width;
    int32_t clip_y2 = win->clip_rect.y + (int32_t)win->clip_rect.height;

    int32_t draw_x1 = win->bounds.x + rect->x;
    int32_t draw_y1 = win->bounds.y + rect->y;
    int32_t draw_x2 = draw_x1 + (int32_t)rect->width;
    int32_t draw_y2 = draw_y1 + (int32_t)rect->height;

    if (draw_x1 < clip_x1) draw_x1 = clip_x1;
    if (draw_y1 < clip_y1) draw_y1 = clip_y1;
    if (draw_x2 > clip_x2) draw_x2 = clip_x2;
    if (draw_y2 > clip_y2) draw_y2 = clip_y2;

    if (draw_x1 >= draw_x2 || draw_y1 >= draw_y2) {
        az_spinlock_release(&g_win32k_lock);
        return AZ_STATUS_SUCCESS;
    }

    az_object_t* current_thread = NULL;
    az_status_t status = az_scheduler_get_current_thread(&current_thread);
    if (AZ_ERROR(status) || current_thread == NULL || current_thread->data.thread.process_obj == NULL) {
        if (current_thread != NULL) az_object_dereference(current_thread);
        az_spinlock_release(&g_win32k_lock);
        return AZ_STATUS_INVALID_HANDLE;
    }

    az_process_t* proc = &current_thread->data.thread.process_obj->data.process;
    az_object_t* surf_obj = NULL;
    status = az_handle_lookup(proc->handle_table, win->surface_handle, AZ_OBJ_TYPE_SURFACE, &surf_obj);
    az_object_dereference(current_thread);

    if (AZ_ERROR(status) || surf_obj == NULL) {
        az_spinlock_release(&g_win32k_lock);
        return AZ_STATUS_INVALID_HANDLE;
    }

    az_spinlock_acquire(&surf_obj->lock);
    az_surface_t* surf = &surf_obj->data.surface;
    uint32_t back_idx = surf->active_page_index ^ 1U;
    uint8_t* vram = surf->framebuffer_pages[back_idx];
    uint32_t pitch = surf->pitch;

    for (int32_t y = draw_y1; y < draw_y2; y++) {
        if (y < 0 || y >= (int32_t)surf->height) continue;
        uint32_t* row = (uint32_t*)(vram + ((uint32_t)y * pitch));
        for (int32_t x = draw_x1; x < draw_x2; x++) {
            if (x < 0 || x >= (int32_t)surf->width) continue;
            row[x] = color;
        }
    }

    az_spinlock_release(&surf_obj->lock);
    az_object_dereference(surf_obj);
    az_spinlock_release(&g_win32k_lock);

    return AZ_STATUS_SUCCESS;
}

/* =========================================================================
 * 2D WINDOW COMPOSITION ENGINE
 * ========================================================================= */

az_status_t az_win32k_composite_all_windows(az_object_t* target_framebuffer_surface) {
    if (target_framebuffer_surface == NULL || target_framebuffer_surface->type != AZ_OBJ_TYPE_SURFACE) {
        return AZ_STATUS_INVALID_PARAMETER;
    }

    az_spinlock_acquire(&target_framebuffer_surface->lock);
    az_surface_t* fb_surf = &target_framebuffer_surface->data.surface;
    uint32_t back_idx = fb_surf->active_page_index ^ 1U;
    uint8_t* target_vram = fb_surf->framebuffer_pages[back_idx];
    uint32_t target_pitch = fb_surf->pitch;

    for (uint32_t y = 0U; y < fb_surf->height; y++) {
        uint32_t* row = (uint32_t*)(target_vram + (y * target_pitch));
        for (uint32_t x = 0U; x < fb_surf->width; x++) {
            row[x] = 0xFF0A0A0AU;
        }
    }
    az_spinlock_release(&target_framebuffer_surface->lock);

    az_spinlock_acquire(&g_win32k_lock);

    for (uint32_t z = 0U; z < 256U; z++) {
        for (size_t i = 0U; i < AZ_WIN32K_MAX_WINDOWS; i++) {
            az_window_t* win = &g_windows[i];
            if (!win->is_allocated || !win->is_visible || win->z_order != z) {
                continue;
            }

            int32_t x1 = win->bounds.x;
            int32_t y1 = win->bounds.y;
            int32_t x2 = x1 + (int32_t)win->bounds.width;
            int32_t y2 = y1 + (int32_t)win->bounds.height;

            if (x1 < 0) x1 = 0;
            if (y1 < 0) y1 = 0;
            if (x2 > (int32_t)fb_surf->width) x2 = (int32_t)fb_surf->width;
            if (y2 > (int32_t)fb_surf->height) y2 = (int32_t)fb_surf->height;

            for (int32_t py = y1; py < y2; py++) {
                uint32_t* dst_row = (uint32_t*)(target_vram + ((uint32_t)py * target_pitch));
                for (int32_t px = x1; px < x2; px++) {
                    dst_row[px] = win->background_color;
                }
            }
        }
    }

    az_spinlock_release(&g_win32k_lock);
    return AZ_STATUS_SUCCESS;
}
