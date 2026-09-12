/* ============================================================================
 * AzamiOS — Security Hardening Implementation
 * File: kernel/security/security.c
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "security.h"
#include "../../arch/x86_64/cpu/cpu.h"
#include "../../arch/x86_64/cpu/mce.h"
#include "../../arch/x86_64/cpu/mitigations.h"
#include "../../arch/x86_64/cpu/msr.h"
#include "../../drivers/char/console.h"
#include "../../include/azami/defs.h"
#include "../lib/random.h"
#include "../syscall/syscall.h"


/* Global stack canary guard value. The compile-time value is a placeholder
 * that must never survive boot: a canary an attacker can read out of the
 * kernel image is not a canary at all. security_init() replaces it before any
 * process runs. */
/* used: every reference to these two symbols is injected by the compiler's
 * stack-protector pass, into every OTHER translation unit, during each
 * TU's own final codegen — after LTO's whole-program reachability analysis
 * has already run. From that analysis's viewpoint nothing calls or reads
 * these yet, so under -flto they get discarded and every other file's
 * canary check turns into an undefined reference at final link. */
__attribute__((used))
uintptr_t __stack_chk_guard = 0x595E9FBD94FDA766ULL;

/* Security configuration knobs (also exposed through /proc/sys) */
u64 g_mmap_min_addr        = 0x10000ULL;
u32 g_dmesg_restrict       = 1;
u32 g_kptr_restrict        = 1;
u32 g_yama_ptrace_scope    = 1;
u32 g_protected_hardlinks  = 1;
u32 g_protected_symlinks   = 1;

/* Whether the guard actually got replaced with unpredictable bits, so the
 * report below can say so rather than implying a protection we do not have. */
static bool s_canary_random = false;

/* no_stack_protector: this function overwrites __stack_chk_guard itself
 * partway through. A protected function's epilogue checks its saved canary
 * against the *current* global, not the one in effect at its own prologue —
 * so if this function carried a canary, changing the guard mid-body would
 * make its own return look like a stack smash (the compiled-in placeholder
 * was pushed at entry; the freshly-randomized value is what the epilogue
 * compares against). Every function called from here runs entirely after
 * the reseed and stays fully protected; this is the one frame that has to
 * opt out, the same way glibc's own stack-chk-guard setup does. */
void security_init(void) __attribute__((no_stack_protector));
void security_init(void)
{
    /* cpu_rand64() prefers RDSEED over RDRAND and retries both, where this
     * used to take a single unchecked RDRAND and silently keep the compiled-in
     * constant when it failed. The CSPRNG is the fallback rather than the
     * first choice deliberately: it is already seeded by this point, but
     * drawing from it here would shift the stream that AT_RANDOM and the ELF
     * loader's ASLR offsets consume, making every process layout in the system
     * depend on how this one value was obtained. */
    u64 guard = 0;
    if (!cpu_rand64(&guard)) guard = krandom_u64();

    if (guard != 0) {
        /* Zero the low byte. A stack canary with a NUL in it terminates the
         * string overflows that are the main thing canaries catch, so the
         * overflow cannot copy the canary forward intact. Costs 8 bits of
         * entropy and buys immunity to the standard read-then-rewrite bypass. */
        __stack_chk_guard = (uintptr_t)(guard & ~0xFFULL);
        s_canary_random = true;
    }

    pr_debug("[SECURITY] Stack canary guard active: 0x%016llx (%s)\n",
            (unsigned long long)__stack_chk_guard,
            s_canary_random ? "random" : "FIXED — no entropy source");

    /* The control registers are the foundation everything else here rests on:
     * SMEP stops ring 0 executing user pages, SMAP stops it reading them by
     * accident, CR0.WP makes .rodata and the page tables actually read-only.
     * Confirm the boot path left them the way it claimed to. */
    if (!cpu_check_control_regs("security_init"))
        kprintf("[SECURITY] control registers were repaired during init\n");

    char line[192];
    mitigations_format_short(line, sizeof line);
    pr_debug("[SECURITY] %s\n", line);
}

/* ── Boot-time security posture, for /proc and for the boot log ──────────── */

size_t security_format_status(char *buf, size_t max)
{
    extern int scnprintf(char *b, size_t n, const char *fmt, ...);
    size_t off = 0;

    #define ROW(name, val) \
        off += scnprintf(buf + off, max > off ? max - off : 0, \
                         "%-22s %s\n", name ":", val)

    ROW("smep",           g_smep_enabled     ? "enabled" : "unavailable");
    ROW("smap",           g_smap_enabled     ? "enabled" : "unavailable");
    ROW("umip",           g_umip_enabled     ? "enabled" : "unavailable");
    ROW("nx",             (rdmsr(MSR_EFER) & EFER_NXE) ? "enabled" : "disabled");
    ROW("cr0_wp",         (read_cr0() & CR0_WP) ? "enabled" : "DISABLED");
    ROW("rdpmc_user",     (read_cr4() & CR4_PCE) ? "ALLOWED" : "denied");
    ROW("pku",            g_pku_enabled      ? "enabled" : "unavailable");
    ROW("stack_canary",   s_canary_random    ? "random per boot" : "FIXED");
    ROW("machine_check",  g_mce_enabled      ? "enabled" : "unavailable");
    ROW("split_lock",     g_split_lock_detect? "detecting" : "off");
    ROW("cet_shadow_stack",
        g_cpu_info.has_shstk ? "supported, not enabled" : "unsupported");
    ROW("cet_ibt",
        g_cpu_info.has_ibt   ? "supported, not enabled" : "unsupported");

    off += scnprintf(buf + off, max > off ? max - off : 0,
                     "%-22s 0x%016llx\n", "cr4_pinned:",
                     (unsigned long long)g_cr4_pinned);
    off += scnprintf(buf + off, max > off ? max - off : 0,
                     "%-22s %llu\n", "split_lock_faults:",
                     (unsigned long long)cpu_split_lock_count());
    off += scnprintf(buf + off, max > off ? max - off : 0,
                     "%-22s 0x%llx\n", "mmap_min_addr:",
                     (unsigned long long)g_mmap_min_addr);
    ROW("dmesg_restrict",      g_dmesg_restrict ? "enabled" : "disabled");
    ROW("kptr_restrict",       g_kptr_restrict ? "enabled" : "disabled");
    ROW("protected_hardlinks", g_protected_hardlinks ? "enabled" : "disabled");
    ROW("protected_symlinks",  g_protected_symlinks ? "enabled" : "disabled");
    off += scnprintf(buf + off, max > off ? max - off : 0,
                     "%-22s %u\n", "yama_ptrace_scope:",
                     (unsigned int)g_yama_ptrace_scope);
    #undef ROW
    return off;
}

__attribute__((used))
__noreturn void __stack_chk_fail(void)
{
    kprintf("[SECURITY] canary check failed, called from %p (frame %p)\n",
            __builtin_return_address(0), __builtin_frame_address(0));
    PANIC("KERNEL SECURITY VIOLATION: Stack Canary Check Failed! (Buffer Overflow Detected)");
}

bool security_validate_user_ptr(const void *ptr, size_t size)
{
    uintptr_t addr = (uintptr_t)ptr;
    /* User space must reside below the canonical hole (< 0x00007FFFFFFFFFFF) */
    if (addr >= 0x0000800000000000ULL) return false;
    if (addr + size < addr || addr + size >= 0x0000800000000000ULL) return false;
    return true;
}

bool security_validate_kernel_ptr(const void *ptr, size_t size)
{
    uintptr_t addr = (uintptr_t)ptr;
    /* Kernel space must reside in the higher half (>= 0xFFFF800000000000) */
    if (addr < 0xFFFF800000000000ULL) return false;
    if (addr + size < addr) return false;
    return true;
}

/* ── POSIX.1e capability engine ───────────────────────────────────────────
 * The previous implementation ignored `capability` entirely and answered "is
 * this euid 0", which meant a privilege could neither be delegated to a
 * non-root service nor dropped by a root one. The sets below make each CAP_*
 * check independent, while preserving the traditional rule that uid 0 carries
 * the full set — so existing root userland behaves exactly as before.
 * ------------------------------------------------------------------------ */

bool security_check_permission(process_t *proc, u32 capability)
{
    if (!proc) return false;
    if (capability > CAP_LAST_CAP) return false;   /* unknown cap: never grant */

    /* pid 0 (kernel) and pid 1 (init) are the trusted base of the system and
     * predate any credential setup. */
    if (proc->pid == 0 || proc->pid == 1) return true;

    return (proc->cap_effective & CAP_TO_MASK(capability)) != 0;
}

void security_caps_init(process_t *proc, bool is_first)
{
    if (!proc) return;

    /* The bounding set starts complete for everyone: it is a ceiling that only
     * ever shrinks, and narrowing it is what makes a sandbox irreversible. */
    proc->cap_bounding    = CAP_FULL_SET;
    proc->cap_inheritable = 0;

    if (is_first || proc->euid == 0) {
        proc->cap_permitted = CAP_FULL_SET;
        proc->cap_effective = CAP_FULL_SET;
    } else {
        proc->cap_permitted = 0;
        proc->cap_effective = 0;
    }

    proc->no_new_privs = false;
    proc->seccomp_mode = SECCOMP_MODE_DISABLED;
}

void security_caps_on_setuid(process_t *proc)
{
    if (!proc) return;

    if (proc->euid == 0) {
        /* Gaining root grants everything the bounding set still allows. A
         * process that narrowed its bounding set cannot climb back out by
         * transitioning to uid 0. */
        proc->cap_permitted |= (CAP_FULL_SET & proc->cap_bounding);
        proc->cap_effective  = proc->cap_permitted;
    } else {
        /* Dropping root drops the privileges that came with it. Anything the
         * process was granted explicitly (and is still within the bounding
         * set) survives, which is what lets a service shed uid 0 but keep,
         * say, CAP_NET_BIND_SERVICE. */
        proc->cap_permitted &= proc->cap_bounding;
        proc->cap_effective &= proc->cap_permitted;
    }
}

void security_caps_on_exec(process_t *proc)
{
    if (!proc) return;

    /* No file capabilities and no setuid-on-exec support yet, so the only way
     * to hold privilege across exec is to already be root. Under
     * no_new_privs the process must not gain anything at all — that is the
     * whole guarantee the flag makes to a sandbox that set it. */
    if (proc->euid == 0 && !proc->no_new_privs) {
        proc->cap_permitted = CAP_FULL_SET & proc->cap_bounding;
    } else {
        proc->cap_permitted &= (proc->cap_inheritable | proc->cap_permitted);
        proc->cap_permitted &= proc->cap_bounding;
    }
    proc->cap_effective = proc->cap_permitted;
}

/* Syscalls a SECCOMP_MODE_STRICT process may still make. Linux allows exactly
 * read, write, _exit and sigreturn; exit_group is included because that is how
 * a normal C runtime terminates and omitting it would turn every strict-mode
 * exit into a SIGKILL. */
bool security_seccomp_check(process_t *proc, u64 syscall_nr)
{
    if (!proc || proc->seccomp_mode == SECCOMP_MODE_DISABLED) return true;

    if (proc->seccomp_mode == SECCOMP_MODE_STRICT) {
        switch (syscall_nr) {
        case SYS_read:
        case SYS_write:
        case SYS_exit:
        case SYS_exit_group:
        case SYS_rt_sigreturn:
            return true;
        default:
            return false;
        }
    }
    return true;
}
