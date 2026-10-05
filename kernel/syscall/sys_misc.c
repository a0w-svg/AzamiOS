/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/* ============================================================================
 * AzamiOS — Miscellaneous and System Information Syscalls
 * File: kernel/syscall/sys_misc.c
 * ============================================================================ */
#include "syscall_internal.h"


s64 sys_sysinfo_impl(pt_regs_t *r)
{
    void *user_info = (void *)r->rdi;
    if (!user_info) return -(s64)EINVAL;
    
    struct {
        long uptime;
        unsigned long loads[3];
        unsigned long totalram;
        unsigned long freeram;
        unsigned long sharedram;
        unsigned long bufferram;
        unsigned long totalswap;
        unsigned long freeswap;
        unsigned short procs;
        unsigned short pad;
        unsigned long totalhigh;
        unsigned long freehigh;
        unsigned int mem_unit;
        char _f[20-2*sizeof(long)-sizeof(int)];
    } info;
    
    __builtin_memset(&info, 0, sizeof(info));
    info.uptime = (long)(sched_get_ticks() / 100);
    info.mem_unit = 4096;
    info.totalram = pmm_get_total_pages();
    info.freeram = pmm_get_free_pages();
    info.procs = (unsigned short)sched_get_process_count();
    
    if (copy_to_user(user_info, &info, sizeof(info)) != 0) return -(s64)EFAULT;
    return 0;
}

s64 sys_az_sysstat_impl(pt_regs_t *r)
{
    az_sysstat_t *user_stat = (az_sysstat_t *)r->rdi;
    if (!user_stat) return -(s64)EINVAL;
    
    az_sysstat_t stat;
    __builtin_memset(&stat, 0, sizeof(stat));
    
    for (int i = 0; i < 16; i++) {
        stat.idle_ticks[i] = sched_get_idle_ticks((u32)i);
        stat.active_ticks[i] = sched_get_active_ticks((u32)i);
    }
    
    if (copy_to_user(user_stat, &stat, sizeof(stat)) != 0) return -(s64)EFAULT;
    return 0;
}

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

static char g_kernel_nodename[65] = "azamios";
static char g_kernel_domainname[65] = "local";

s64 sys_uname_impl(pt_regs_t *r)
{
    struct utsname *u = (struct utsname *)r->rdi;
    if (!u || (uintptr_t)u >= TASK_SIZE_MAX) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    const char *nodename = (proc && proc->uts_ns && proc->uts_ns->nodename[0])
                           ? proc->uts_ns->nodename : g_kernel_nodename;
    const char *domainname = (proc && proc->uts_ns && proc->uts_ns->domainname[0])
                             ? proc->uts_ns->domainname : g_kernel_domainname;

    struct utsname info;
    memset(&info, 0, sizeof(info));
    strncpy(info.sysname, "AzamiOS", sizeof(info.sysname) - 1);
    strncpy(info.nodename, nodename, sizeof(info.nodename) - 1);
    strncpy(info.release, "7.0.0-posix", sizeof(info.release) - 1);
    strncpy(info.version, "AzamiOS Modular Microkernel v7.0 x86_64 SMP", sizeof(info.version) - 1);
    strncpy(info.machine, "x86_64", sizeof(info.machine) - 1);
    strncpy(info.domainname, domainname, sizeof(info.domainname) - 1);

    if (copy_to_user(u, &info, sizeof(info)) != 0) return -(s64)EFAULT;
    return 0;
}

s64 sys_reboot_impl(pt_regs_t *r)
{
    process_t *proc = sched_current_process();
    if (!security_check_permission(proc, CAP_SYS_BOOT)) {
        return -(s64)EPERM;
    }
    u32 cmd = (u32)r->rdx;
    /* Last chance to get buffered filesystem writes onto the platter. */
    vfs_sync_all();
    if (cmd == 0x01234567 /* LINUX_REBOOT_CMD_RESTART */) {
        power_reboot();
    } else {
        extern __attribute__((noreturn)) void acpi_shutdown(void);
        acpi_shutdown();
    }
    __builtin_unreachable();
}

extern virtio_gpu_state_t g_gpu;

s64 sys_az_fb_info(pt_regs_t *r)
{
    az_fb_info_t *user_info = (az_fb_info_t *)r->rdi;
    if (!user_info || (uintptr_t)user_info >= TASK_SIZE_MAX) return -(s64)EFAULT;

    az_fb_info_t info;
    __builtin_memset(&info, 0, sizeof(info));

    /* Same backend priority as drivers/video/fbdev.c's fbdev_init(): BGA
     * first, then VirtIO-GPU, then the Limine boot GOP framebuffer last —
     * when a virtio-gpu is present, QEMU may also have exposed a std-VGA
     * whose Limine GOP aperture points at *different* memory with a
     * *different* pitch, so checking it first would hand back geometry that
     * does not describe the surface azwm and every client actually end up
     * mapped to via /dev/fb0. Before this matched only BGA-or-GOP, every
     * caller of az_fb_info() (azwm's own screen_w/h/pitch, and every client
     * app's window centring) silently got the wrong pitch under virtio-gpu —
     * right width/height by coincidence (same negotiated mode), wrong
     * stride, which is what a sheared/garbled-looking composite is. */
    phys_addr_t bga_phys = bga_get_fb_phys();
    if (bga_phys) {
        info.width     = bga_get_width();
        info.height    = bga_get_height();
        info.pitch     = bga_get_pitch();
        info.bpp       = bga_get_bpp();
        info.phys_addr = bga_phys;
    } else if (g_gpu.framebuffer_phys != 0) {
        info.width     = g_gpu.screen_width  ? g_gpu.screen_width  : 1280;
        info.height    = g_gpu.screen_height ? g_gpu.screen_height : 800;
        info.pitch     = info.width * 4;
        info.bpp       = 32;
        info.phys_addr = g_gpu.framebuffer_phys;
    } else {
        struct limine_framebuffer *fb = az_boot_framebuffer();
        if (!fb) return -(s64)ENODEV;
        info.width     = (u32)fb->width;
        info.height    = (u32)fb->height;
        info.pitch     = (u32)fb->pitch;
        info.bpp       = (u8)fb->bpp;
        info.phys_addr = (u64)(uintptr_t)fb->address - HHDM_BASE;
    }

    if (copy_to_user(user_info, &info, sizeof(az_fb_info_t)) != 0) return -(s64)EFAULT;
    return 0;
}

s64 sys_az_fb_map(pt_regs_t *r)
{
    virt_addr_t virt = (virt_addr_t)r->rdi;
    if (virt & (PAGE_SIZE - 1)) return -(s64)EINVAL;
    if (virt >= TASK_SIZE_MAX) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    phys_addr_t fb_phys = bga_get_fb_phys();
    size_t      fb_size = 0;

    if (fb_phys) {
        fb_size = bga_get_fb_total_size();
    } else if (g_gpu.framebuffer_phys != 0) {
        /* Same backend priority as sys_az_fb_info() / fbdev.c — see the
         * comment there. This path is only reached by a caller whose
         * /dev/fb0 open failed (map_shared_memory()'s fallback), but it
         * needs to be virtio-gpu-aware for the same reason: mapping the
         * Limine boot GOP aperture instead of the actual scanout resource
         * would hand back memory the compositor never reads from. */
        fb_phys = g_gpu.framebuffer_phys;
        fb_size = g_gpu.framebuffer_size;
    } else {
        struct limine_framebuffer *fb = az_boot_framebuffer();
        if (!fb) return -(s64)ENODEV;
        fb_phys = (phys_addr_t)((u64)(uintptr_t)fb->address - HHDM_BASE);
        fb_size = (size_t)(fb->pitch * fb->height);
    }

    size_t page_count = (fb_size + PAGE_SIZE - 1) / PAGE_SIZE;
    u64 flags = VMM_F_PRESENT | VMM_F_WRITE | VMM_F_USER | VMM_F_NX | VMM_F_SHARED | VMM_F_PWT;
    for (size_t i = 0; i < page_count; i++) {
        vmm_map(proc->pml4_phys, virt + i * PAGE_SIZE, fb_phys + i * PAGE_SIZE, flags);
    }

    console_disable_fb();
    return 0;
}

s64 sys_az_fb_flip(pt_regs_t *r)
{
    u32 buffer_index = (u32)r->rdi;
    if (buffer_index > 1) return -(s64)EINVAL;
    if (bga_get_fb_phys() != 0) {
        if (bga_flip_buffer(buffer_index) == 0) return 0;
    }
    return -(s64)ENOSYS;
}

s64 sys_seccomp_impl(pt_regs_t *r)
{
    unsigned int op    = (unsigned int)r->rdi;
    unsigned int flags = (unsigned int)r->rsi;
    process_t *p = sched_current_process();
    if (!p) return -(s64)EPERM;

    switch (op) {
    case SECCOMP_SET_MODE_STRICT:
        /* Linux takes no flags and no args for strict mode. */
        if (flags != 0 || r->rdx != 0) return -(s64)EINVAL;
        if (p->seccomp_mode != SECCOMP_MODE_DISABLED &&
            p->seccomp_mode != SECCOMP_MODE_STRICT) return -(s64)EINVAL;
        /* Entering strict mode implies no_new_privs: without it an execve()
         * could hand the process a setuid binary running under the filter. */
        p->no_new_privs  = true;
        p->seccomp_mode  = SECCOMP_MODE_STRICT;
        return 0;

    case SECCOMP_SET_MODE_FILTER:
        if (flags != SECCOMP_FILTER_FLAG_NONE) return -(s64)EINVAL;
        if (p->seccomp_mode != SECCOMP_MODE_DISABLED &&
            p->seccomp_mode != SECCOMP_MODE_FILTER) return -(s64)EINVAL;
        return seccomp_attach_filter(p, (const sock_fprog_t *)r->rdx);

    case SECCOMP_GET_ACTION_AVAIL: {
        if (flags != 0) return -(s64)EINVAL;
        u32 act = 0;
        if (copy_from_user(&act, (const void *)r->rdx, sizeof(u32)) != 0)
            return -(s64)EFAULT;
        switch (act) {
        case SECCOMP_RET_KILL_THREAD:
        case SECCOMP_RET_KILL_PROCESS:
        case SECCOMP_RET_TRAP:
        case SECCOMP_RET_ERRNO:
        case SECCOMP_RET_LOG:
        case SECCOMP_RET_ALLOW:
            return 0;
        /* SECCOMP_RET_TRACE is deliberately not reported available: it
         * needs a ptrace tracer to be notified and to decide the outcome
         * (PTRACE_EVENT_SECCOMP), which this kernel does not wire up yet.
         * seccomp_filter_run() fails a TRACE result closed (-ENOSYS to the
         * caller) rather than silently allowing it, but that is not the
         * same guarantee as the real action, so it is not advertised here. */
        default:
            return -(s64)95; /* -EOPNOTSUPP */
        }
    }

    default:
        return -(s64)EINVAL;
    }
}

s64 sys_rseq_impl(pt_regs_t *r)
{
    (void)r;
    return -(s64)ENOSYS;
}



s64 sys_getrandom_impl(pt_regs_t *r)
{
    void *buf = (void *)r->rdi;
    size_t buflen = (size_t)r->rsi;
    unsigned int flags = (unsigned int)r->rdx;

    /* GRND_NONBLOCK=1, GRND_RANDOM=2, GRND_INSECURE=4 */
    if (flags & ~0x7u) return -(s64)EINVAL;
    if ((flags & 0x2u) && (flags & 0x4u)) return -(s64)EINVAL;
    if (!buf && buflen) return -(s64)EFAULT;
    if (buflen == 0) return 0;
    if ((uintptr_t)buf >= TASK_SIZE_MAX) return -(s64)EFAULT;

    /* The kernel CSPRNG is always seeded early in boot, so GRND_NONBLOCK never
     * needs to return EAGAIN and GRND_RANDOM does not block. */
    u8 kbuf[256];
    size_t written = 0;
    while (written < buflen) {
        size_t chunk = buflen - written;
        if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
        krandom_bytes(kbuf, chunk);
        if (copy_to_user((char *)buf + written, kbuf, chunk) != 0) {
            __builtin_memset(kbuf, 0, sizeof(kbuf));
            return written ? (s64)written : -(s64)EFAULT;
        }
        written += chunk;
    }
    __builtin_memset(kbuf, 0, sizeof(kbuf));
    return (s64)buflen;
}

s64 sys_syslog_impl(pt_regs_t *r)
{
    int type = (int)r->rdi;
    char *user_buf = (char *)r->rsi;
    int len = (int)r->rdx;

    process_t *proc = sched_current_process();
    bool has_priv = proc && (proc->euid == 0 ||
                             security_check_permission(proc, CAP_SYSLOG) ||
                             security_check_permission(proc, CAP_SYS_ADMIN));

    /* Privileged syslog actions always require CAP_SYSLOG or CAP_SYS_ADMIN */
    if (type == 1 || type == 5 || type == 6 || type == 7 || type == 8) {
        if (!has_priv) return -(s64)EPERM;
    }

    /* Reading or sizing kernel log buffer requires privilege when dmesg_restrict is enabled */
    if (g_dmesg_restrict && !has_priv) {
        return -(s64)EPERM;
    }

    extern s64 console_read_klog(void *buf, size_t max_len, u64 *offset);
    extern u64 console_get_klog_size(void);

    switch (type) {
    case 0:
    case 1:
    case 5:
    case 6:
    case 7:
    case 8:
        return 0;
    case 2:
    case 3:
    case 4: {
        if (!user_buf || len <= 0) return -(s64)EINVAL;
        char *kbuf = (char *)kmalloc((size_t)len);
        if (!kbuf) return -(s64)ENOMEM;
        u64 offset = 0;
        s64 n = console_read_klog(kbuf, (size_t)len, &offset);
        if (n > 0) {
            if (copy_to_user(user_buf, kbuf, (size_t)n) != 0) {
                kfree(kbuf);
                return -(s64)EFAULT;
            }
        }
        kfree(kbuf);
        return n;
    }
    case 9: {
        u64 sz = console_get_klog_size();
        return (s64)(sz > 65536 ? 65536 : sz);
    }
    case 10:
        return 65536;
    default:
        return -(s64)EINVAL;
    }
}

/* MEMBARRIER_CMD_* (include/uapi/linux/membarrier.h). Only the commands this
 * kernel can actually honour are advertised by CMD_QUERY below. */
#define MEMBARRIER_CMD_QUERY                        0
#define MEMBARRIER_CMD_GLOBAL                       (1 << 0)
#define MEMBARRIER_CMD_GLOBAL_EXPEDITED             (1 << 1)
#define MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED    (1 << 2)
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED            (1 << 3)
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED   (1 << 4)

/* Runs on the target CPU via the cross-CPU call. A full barrier is all that
 * is owed: the IPI's own entry and exit are already serialising events on
 * x86, so this is belt-and-braces made explicit rather than the mechanism. */
static void membarrier_ipi(void *unused)
{
    (void)unused;
    __asm__ volatile("mfence" ::: "memory");
}

/*
 * membarrier(2).
 *
 * The contract is that on return, every other CPU that could be running a
 * thread of this process has executed a full memory barrier — which is what
 * lets userspace put the expensive side of an asymmetric barrier (an RCU
 * read-side critical section, a seqlock reader, a lock-free queue's fast
 * path) entirely in the kernel and leave the hot path a plain load.
 *
 * This used to return -ENOSYS, on the reasoning that the honest failure was
 * better than a local fence pretending to be a global one. That was the right
 * call at the time: there was no way to make another CPU execute anything.
 * smp_call_function_mask() is that way, so the syscall can now do what it
 * says. PRIVATE_EXPEDITED targets only the CPUs this process is on (what
 * sched_proc_cpu_mask() computes); GLOBAL targets every online CPU.
 */
s64 sys_membarrier_impl(pt_regs_t *r)
{
    int cmd = (int)(s32)r->rdi;
    u32 flags = (u32)r->rsi;

    if (flags != 0) return -(s64)EINVAL;

    switch (cmd) {
    case MEMBARRIER_CMD_QUERY:
        return MEMBARRIER_CMD_GLOBAL |
               MEMBARRIER_CMD_GLOBAL_EXPEDITED |
               MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED |
               MEMBARRIER_CMD_PRIVATE_EXPEDITED |
               MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED;

    /* Registration is a no-op here: nothing is deferred or batched, so there
     * is no state to pre-arm. Returning success is correct — the commands the
     * registration enables all work. */
    case MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED:
    case MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED:
        return 0;

    case MEMBARRIER_CMD_PRIVATE_EXPEDITED: {
        process_t *p = sched_current_process();
        if (!p) return -(s64)EPERM;
        u64 mask = sched_proc_cpu_mask(p);
        __asm__ volatile("mfence" ::: "memory");
        if (mask) smp_call_function_mask(mask, membarrier_ipi, NULL, true);
        return 0;
    }

    case MEMBARRIER_CMD_GLOBAL:
    case MEMBARRIER_CMD_GLOBAL_EXPEDITED:
        __asm__ volatile("mfence" ::: "memory");
        smp_call_function(membarrier_ipi, NULL, true);
        return 0;

    default:
        return -(s64)EINVAL;
    }
}

s64 sys_sethostname_impl(pt_regs_t *r)
{
    const char *name = (const char *)r->rdi;
    size_t len = (size_t)r->rsi;
    process_t *proc = sched_current_process();
    if (!security_check_permission(proc, CAP_SYS_ADMIN)) return -(s64)EPERM;
    if (!name || (uintptr_t)name >= TASK_SIZE_MAX || len >= sizeof(g_kernel_nodename)) return -(s64)EINVAL;
    char buf[65];
    memset(buf, 0, sizeof(buf));
    if (copy_from_user(buf, name, len) != 0) return -(s64)EFAULT;
    buf[len] = '\0';
    if (proc && proc->uts_ns) {
        strncpy(proc->uts_ns->nodename, buf, sizeof(proc->uts_ns->nodename) - 1);
        proc->uts_ns->nodename[sizeof(proc->uts_ns->nodename) - 1] = '\0';
    } else {
        strncpy(g_kernel_nodename, buf, sizeof(g_kernel_nodename) - 1);
        g_kernel_nodename[sizeof(g_kernel_nodename) - 1] = '\0';
    }
    return 0;
}

s64 sys_setdomainname_impl(pt_regs_t *r)
{
    const char *name = (const char *)r->rdi;
    size_t len = (size_t)r->rsi;
    process_t *proc = sched_current_process();
    if (!security_check_permission(proc, CAP_SYS_ADMIN)) return -(s64)EPERM;
    if (!name || (uintptr_t)name >= TASK_SIZE_MAX || len >= sizeof(g_kernel_domainname)) return -(s64)EINVAL;
    char buf[65];
    memset(buf, 0, sizeof(buf));
    if (copy_from_user(buf, name, len) != 0) return -(s64)EFAULT;
    buf[len] = '\0';
    if (proc && proc->uts_ns) {
        strncpy(proc->uts_ns->domainname, buf, sizeof(proc->uts_ns->domainname) - 1);
        proc->uts_ns->domainname[sizeof(proc->uts_ns->domainname) - 1] = '\0';
    } else {
        strncpy(g_kernel_domainname, buf, sizeof(g_kernel_domainname) - 1);
        g_kernel_domainname[sizeof(g_kernel_domainname) - 1] = '\0';
    }
    return 0;
}

/* ============================================================================
 * iopl(2) / ioperm(2)
 * ========================================================================= */

s64 sys_iopl_impl(pt_regs_t *r)
{
    unsigned int level = (unsigned int)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (level > 3) return -(s64)EINVAL;
    if (proc->euid != 0 && !security_check_permission(proc, CAP_SYS_RAWIO)) {
        return -(s64)EPERM;
    }
    r->rflags = (r->rflags & ~0x3000ULL) | (((u64)level & 3) << 12);
    return 0;
}

s64 sys_ioperm_impl(pt_regs_t *r)
{
    (void)r;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (proc->euid != 0 && !security_check_permission(proc, CAP_SYS_RAWIO)) {
        return -(s64)EPERM;
    }
    return 0;
}

/* ============================================================================
 * acct(2)
 * ========================================================================= */

s64 sys_acct_impl(pt_regs_t *r)
{
    const char *filename = (const char *)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (proc->euid != 0 && !security_check_permission(proc, CAP_SYS_PACCT)) {
        return -(s64)EPERM;
    }
    if (!filename) return 0;
    return -(s64)ENOSYS;
}

/* ============================================================================
 * init_module(2) / delete_module(2) / finit_module(2)
 * ========================================================================= */

s64 sys_finit_module_impl(pt_regs_t *r)
{
    (void)r;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (proc->euid != 0 && !security_check_permission(proc, CAP_SYS_MODULE)) {
        return -(s64)EPERM;
    }
    return -(s64)ENOSYS;
}
s64 sys_add_key_impl(pt_regs_t *r)
{
    (void)r;
    /* Linux Keyring: add_key: Currently unsupported, returning ENOSYS. Programs will fall back or handle it. */
    return -(s64)ENOSYS;
}

s64 sys_request_key_impl(pt_regs_t *r)
{
    (void)r;
    /* Linux Keyring: request_key: Currently unsupported, returning ENOSYS. Programs will fall back or handle it. */
    return -(s64)ENOSYS;
}

s64 sys_keyctl_impl(pt_regs_t *r)
{
    (void)r;
    /* Linux Keyring: keyctl: Currently unsupported, returning ENOSYS. Programs will fall back or handle it. */
    return -(s64)ENOSYS;
}

s64 sys_quotactl_fd_impl(pt_regs_t *r)
{
    (void)r;
    /* quotactl_fd: Currently unsupported, returning ENOSYS. Programs will fall back or handle it. */
    return -(s64)ENOSYS;
}

s64 sys_landlock_create_ruleset_impl(pt_regs_t *r)
{
    (void)r;
    /* Landlock LSM: create_ruleset: Currently unsupported, returning ENOSYS. Programs will fall back or handle it. */
    return -(s64)ENOSYS;
}

s64 sys_landlock_add_rule_impl(pt_regs_t *r)
{
    (void)r;
    /* Landlock LSM: add_rule: Currently unsupported, returning ENOSYS. Programs will fall back or handle it. */
    return -(s64)ENOSYS;
}

s64 sys_landlock_restrict_self_impl(pt_regs_t *r)
{
    (void)r;
    /* Landlock LSM: restrict_self: Currently unsupported, returning ENOSYS. Programs will fall back or handle it. */
    return -(s64)ENOSYS;
}

s64 sys_lsm_get_self_attr_impl(pt_regs_t *r)
{
    (void)r;
    /* lsm_get_self_attr: Currently unsupported, returning ENOSYS. Programs will fall back or handle it. */
    return -(s64)ENOSYS;
}

s64 sys_lsm_set_self_attr_impl(pt_regs_t *r)
{
    (void)r;
    /* lsm_set_self_attr: Currently unsupported, returning ENOSYS. Programs will fall back or handle it. */
    return -(s64)ENOSYS;
}

s64 sys_lsm_list_modules_impl(pt_regs_t *r)
{
    (void)r;
    /* lsm_list_modules: Currently unsupported, returning ENOSYS. Programs will fall back or handle it. */
    return -(s64)ENOSYS;
}

/* ============================================================================
 * ktrace syscall stubs — delegate to ktrace.c at kernel/perf/ktrace.c
 * These provide a direct syscall interface as an alternative to the
 * /sys/kernel/trace/ VFS interface.
 * ============================================================================ */

/* ktrace syscall implementations (API defined in kernel/perf/ktrace.h) */

/* ktrace_enable(name_ptr) or ktrace_disable("-name_ptr") */
s64 sys_az_ktrace_enable_impl(pt_regs_t *r)
{
    const char *uname = (const char *)r->rdi;
    char kname[64];
    s64 err = copy_str_from_user(kname, uname, sizeof(kname));
    if (err < 0) return -(s64)EFAULT;

    if (kname[0] == '-') {
        return (s64)ktrace_disable(kname + 1);
    }
    return (s64)ktrace_enable(kname);
}

/* ktrace_read(buf, max_entries) → number of entries written */
s64 sys_az_ktrace_read_impl(pt_regs_t *r)
{
    void  *ubuf       = (void *)r->rdi;
    size_t max_entries = (size_t)r->rsi;
    if (!ubuf || (uintptr_t)ubuf >= TASK_SIZE_MAX) return -(s64)EFAULT;
    if (max_entries == 0) return 0;
    if (max_entries > 256) max_entries = 256;

    size_t sz = max_entries * sizeof(ktrace_entry_t);
    ktrace_entry_t *kbuf = (ktrace_entry_t *)kzalloc(sz);
    if (!kbuf) return -(s64)ENOMEM;

    size_t n = ktrace_read(kbuf, max_entries);
    s64 ret = 0;
    if (n > 0 && copy_to_user(ubuf, kbuf, n * sizeof(ktrace_entry_t)) != 0)
        ret = -(s64)EFAULT;
    else
        ret = (s64)n;

    kfree(kbuf);
    return ret;
}

/* ktrace_clear() */
s64 sys_az_ktrace_clear_impl(pt_regs_t *r)
{
    (void)r;
    ktrace_ring_clear();
    return 0;
}

/* ============================================================================
 * bpf(2), userfaultfd(2), kexec_file_load(2) — ENOSYS stubs
 *
 * These three syscalls have defined numbers but no handler yet:
 *
 *   bpf (321):            Extended BPF VM — a full JIT-compiled in-kernel
 *                         virtual machine for packet filtering, tracing, and
 *                         security policy. Extremely complex to implement
 *                         properly; programs (systemd, containers) probe for
 *                         support with bpf(BPF_PROG_LOAD, ...) and fall back.
 *
 *   userfaultfd (323):    Allocates a file descriptor for handling page faults
 *                         in userspace. Used primarily by QEMU/KVM live
 *                         migration and CRIU checkpoint/restore. Programs
 *                         always check the return value and fall back.
 *
 *   kexec_file_load (320): Boot a new kernel from a file descriptor without
 *                         going through firmware. Requires signed kernel
 *                         image validation. Only used by kexec(8).
 * ============================================================================ */

s64 sys_bpf_impl(pt_regs_t *r)
{
    (void)r;
    return -(s64)ENOSYS;
}

s64 sys_userfaultfd_impl(pt_regs_t *r)
{
    (void)r;
    return -(s64)ENOSYS;
}

s64 sys_kexec_file_load_impl(pt_regs_t *r)
{
    (void)r;
    return -(s64)ENOSYS;
}
