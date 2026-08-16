/* ============================================================================
 * AzamiOS Userspace — IPC & System Services API
 * File: user/libc/include/az/ipc.h
 *
 * User-space wrappers for Azami microkernel IPC, shared memory,
 * framebuffer, input, and process management syscalls.
 * ============================================================================ */
#pragma once

#include "../sys/syscall.h"

/* ── IPC message structure (must match kernel ipc_msg_t layout) ────────────── */
#define AZ_IPC_MSG_MAX_SIZE  256

typedef struct {
    unsigned int  sender_pid;
    unsigned int  msg_type;
    unsigned long length;
    unsigned char data[AZ_IPC_MSG_MAX_SIZE];
} az_ipc_msg_t;

/* ── Framebuffer info structure (must match kernel az_fb_info_t) ───────────── */
typedef struct {
    unsigned int  width;
    unsigned int  height;
    unsigned int  pitch;
    unsigned char bpp;
    unsigned char _pad[3];
    unsigned long phys_addr;
} az_fb_info_t;

/* ── Input event structure (must match kernel input_event_t) ───────────────── */
#define AZ_INPUT_EVENT_NONE   0
#define AZ_INPUT_EVENT_KEY    1
#define AZ_INPUT_EVENT_MOUSE  2

#define AZ_KEY_FLAG_PRESSED   0x01
#define AZ_KEY_FLAG_RELEASED  0x02
#define AZ_KEY_FLAG_SHIFT     0x04
#define AZ_KEY_FLAG_CTRL      0x08
#define AZ_KEY_FLAG_ALT       0x10

#define AZ_MOUSE_BTN_LEFT     0x01
#define AZ_MOUSE_BTN_RIGHT    0x02
#define AZ_MOUSE_BTN_MIDDLE   0x04

typedef struct {
    unsigned char  type;
    unsigned char  keycode;
    unsigned short flags;
    short          mouse_dx;
    short          mouse_dy;
    unsigned char  mouse_buttons;
    unsigned char  scancode;
    char           mouse_dz;
    unsigned char  _pad[1];
    unsigned int   timestamp;
} __attribute__((packed)) az_input_event_t;

/* ── IPC Channel API ──────────────────────────────────────────────────────── */

/** az_channel_create() — Create a new IPC channel. Returns channel ID or negative errno. */
int az_channel_create(void);

/** az_channel_send(channel_id, msg) — Send a message (blocking). Returns 0 or negative errno. */
int az_channel_send(int channel_id, const az_ipc_msg_t *msg);

/** az_channel_send_nb(channel_id, msg) — Send a message (non-blocking). */
int az_channel_send_nb(int channel_id, const az_ipc_msg_t *msg);

/** az_channel_recv(channel_id, msg) — Receive a message (blocking). Returns 0 or negative errno. */
int az_channel_recv(int channel_id, az_ipc_msg_t *msg);

/** az_channel_recv_nb(channel_id, msg) — Receive a message (non-blocking). */
int az_channel_recv_nb(int channel_id, az_ipc_msg_t *msg);

/* ── Shared Memory API ────────────────────────────────────────────────────── */

/** az_shmem_create(pages) — Create shared memory region. Returns shmem_id or negative errno. */
int az_shmem_create(int page_count);

/** az_shmem_map(shmem_id, virt_addr) — Map shared memory into this process. Returns 0 or negative errno. */
int az_shmem_map(int shmem_id, void *virt_addr);

/** az_shmem_unmap(shmem_id, virt_addr) — Unmap shared memory from this process. Returns 0 or negative errno. */
int az_shmem_unmap(int shmem_id, void *virt_addr);

/** az_shmem_destroy(shmem_id) — Destroy shared memory region. Returns 0 or negative errno. */
int az_shmem_destroy(int shmem_id);

/* ── Framebuffer API ──────────────────────────────────────────────────────── */

/** az_fb_info(info) — Get framebuffer dimensions. Returns 0 or negative errno. */
int az_fb_info(az_fb_info_t *info);

/** az_fb_map(virt_addr) — Map the hardware framebuffer at the given address. Returns 0 or negative errno. */
int az_fb_map(void *virt_addr);

/* ── Process API ──────────────────────────────────────────────────────────── */

/** az_spawn(path) — Spawn a new process from an ELF binary. Returns child PID or negative errno. */
int az_spawn(const char *path);

/** az_yield() — Voluntarily yield CPU. */
void az_yield(void);

/** az_sleep(ms) — Sleep for specified milliseconds. */
int az_sleep(unsigned int milliseconds);

/** az_thread_create(entry_fn, stack_top) — Create a new thread in the current process. Returns TID or negative errno. */
int az_thread_create(void *entry_fn, void *stack_top);

/**
 * az_set_timer(channel_id, interval_ms, flags) — Register a kernel timer.
 * The kernel sends an IPC message with msg_type=AZ_WM_TIMER_TICK (51) to
 * channel_id every interval_ms milliseconds.
 * flags: 0 = repeating, 1 = one-shot.
 * Returns 0 on success, negative errno on failure.
 */
int az_set_timer(int channel_id, unsigned int interval_ms, int flags);

/* ── Input API ────────────────────────────────────────────────────────────── */

/** az_input_poll(event) — Poll for one input event. Returns 0 if event received, negative if none. */
int az_input_poll(az_input_event_t *event);
