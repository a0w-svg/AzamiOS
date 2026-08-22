/* ============================================================================
 * AzamiOS Userspace — IPC & System Services Implementation
 * File: user/libc/az_ipc.c
 *
 * Thin syscall wrappers for the Azami microkernel IPC interface.
 * ============================================================================ */

#include "include/az/ipc.h"
#include "include/sys/syscall.h"

/* ── IPC Channels ─────────────────────────────────────────────────────────── */

int az_channel_create(void)
{
    return (int)syscall0(SYS_AZ_CHANNEL_CREATE);
}

int az_channel_send(int channel_id, const az_ipc_msg_t *msg)
{
    return (int)syscall3(SYS_AZ_CHANNEL_SEND, (long)channel_id, (long)msg, 1);
}

int az_channel_send_nb(int channel_id, const az_ipc_msg_t *msg)
{
    return (int)syscall3(SYS_AZ_CHANNEL_SEND, (long)channel_id, (long)msg, 0);
}

int az_channel_recv(int channel_id, az_ipc_msg_t *msg)
{
    return (int)syscall3(SYS_AZ_CHANNEL_RECV, (long)channel_id, (long)msg, 1);
}

int az_channel_recv_nb(int channel_id, az_ipc_msg_t *msg)
{
    return (int)syscall3(SYS_AZ_CHANNEL_RECV, (long)channel_id, (long)msg, 0);
}

/* ── Shared Memory ────────────────────────────────────────────────────────── */

int az_shmem_create(int page_count)
{
    return (int)syscall1(SYS_AZ_SHMEM_CREATE, (long)page_count);
}

int az_shmem_map(int shmem_id, void *virt_addr)
{
    return (int)syscall2(SYS_AZ_SHMEM_MAP, (long)shmem_id, (long)virt_addr);
}

int az_shmem_unmap(int shmem_id, void *virt_addr)
{
    return (int)syscall2(SYS_AZ_SHMEM_UNMAP, (long)shmem_id, (long)virt_addr);
}

int az_shmem_destroy(int shmem_id)
{
    return (int)syscall1(SYS_AZ_SHMEM_DESTROY, (long)shmem_id);
}

#include "include/fcntl.h"
#include "include/unistd.h"
#include "include/sys/ioctl.h"
#include "include/sys/mman.h"
#include "include/linux/fb.h"

/* ── Framebuffer ──────────────────────────────────────────────────────────── */

int az_fb_info(az_fb_info_t *info)
{
    if (!info) return -1;
    int fd = open("/dev/fb0", O_RDONLY);
    if (fd >= 0) {
        struct fb_var_screeninfo var;
        struct fb_fix_screeninfo fix;
        if (ioctl(fd, FBIOGET_VSCREENINFO, &var) == 0 &&
            ioctl(fd, FBIOGET_FSCREENINFO, &fix) == 0) {
            info->width  = var.xres;
            info->height = var.yres;
            info->pitch  = fix.line_length;
            info->bpp    = var.bits_per_pixel;
            close(fd);
            return 0;
        }
        close(fd);
    }
    return (int)syscall1(SYS_AZ_FB_INFO, (long)info);
}

int az_fb_map(void *virt_addr)
{
    int fd = open("/dev/fb0", O_RDWR);
    if (fd >= 0) {
        struct fb_var_screeninfo var;
        struct fb_fix_screeninfo fix;
        if (ioctl(fd, FBIOGET_VSCREENINFO, &var) == 0 &&
            ioctl(fd, FBIOGET_FSCREENINFO, &fix) == 0) {
            size_t len = (size_t)(fix.line_length * (var.yres_virtual ? var.yres_virtual : var.yres * 2));
            if (len == 0) len = (size_t)var.xres * var.yres * 4 * 2;
            int flags = MAP_SHARED;
            if (virt_addr) flags |= MAP_FIXED;
            void *m = mmap(virt_addr, len, PROT_READ | PROT_WRITE, flags, fd, 0);
            close(fd);
            if (m != MAP_FAILED) return 0;
        }
        close(fd);
    }
    return (int)syscall1(SYS_AZ_FB_MAP, (long)virt_addr);
}

int az_fb_flip(unsigned int buffer_index)
{
    int fd = open("/dev/fb0", O_RDWR);
    if (fd >= 0) {
        struct fb_var_screeninfo var;
        if (ioctl(fd, FBIOGET_VSCREENINFO, &var) == 0) {
            var.yoffset = buffer_index * var.yres;
            int ret = ioctl(fd, FBIOPAN_DISPLAY, &var);
            close(fd);
            if (ret == 0) return 0;
        }
        close(fd);
    }
    return (int)syscall1(SYS_AZ_FB_FLIP, (long)buffer_index);
}


/* ── Process Management ───────────────────────────────────────────────────── */

int az_spawn(const char *path)
{
    return (int)syscall1(SYS_AZ_SPAWN, (long)path);
}

void az_yield(void)
{
    syscall0(SYS_AZ_YIELD);
}

struct linux_timespec {
    long tv_sec;
    long tv_nsec;
};

int az_sleep(unsigned int milliseconds)
{
    struct linux_timespec req = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000
    };
    return (int)syscall1(SYS_nanosleep, (long)&req);
}

int az_thread_create(void *entry_fn, void *stack_top)
{
    return (int)syscall2(SYS_AZ_THREAD_CREATE, (long)entry_fn, (long)stack_top);
}


static int g_input_fd = -1;

int az_input_poll(az_input_event_t *event)
{
    if (g_input_fd < 0) {
        g_input_fd = sys_open("/dev/input0", 0, 0); /* 0 = O_RDONLY */
    }
    if (g_input_fd < 0) return -1;
    
    ssize_t ret = sys_read(g_input_fd, event, sizeof(az_input_event_t));
    if (ret == sizeof(az_input_event_t)) return 0;
    
    return -1;
}

/* ── Timer API ────────────────────────────────────────────────────────────── */

int az_set_timer(int channel_id, unsigned int interval_ms, int flags)
{
    return (int)syscall3(SYS_AZ_SET_TIMER,
                         (long)channel_id,
                         (long)(unsigned long)interval_ms,
                         (long)flags);
}
