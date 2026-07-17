/**
 * debugger.c — Kernel Debugger, Symbol Inspection & BSOD Generator Window Service for AzamiOS v6.0
 */
#include "../wm.h"

static char s_dbg_log[12][68];
static int  s_dbg_idx = 0;
static bool s_show_bsod = false;

static void dbg_log(const char *msg) {
    if (s_dbg_idx >= 12) {
        for (int i = 0; i < 11; i++) {
            wm_strlcpy(s_dbg_log[i], s_dbg_log[i + 1], 68);
        }
        s_dbg_idx = 11;
    }
    wm_strlcpy(s_dbg_log[s_dbg_idx++], msg, 68);
}

static void debugger_on_init(window_t *w) {
    if (!w) return;
    wm_resize_window(w, 720, 520);
    s_dbg_idx = 0;
    s_show_bsod = false;
    dbg_log("[DMESG] AzamiOS v6.0 Kernel boot complete.");
    dbg_log("[DMESG] CPU: Intel Core / x86_64 SMP 4 Cores Detected.");
    dbg_log("[DMESG] PMM: 1024 MB Total RAM | 1012 MB Free | Guard pages active.");
    dbg_log("[DMESG] GFX: Intel i915 / Bochs VBE Framebuffer mapped @ 1280x800.");
    dbg_log("[DMESG] SEC: Stack Canary Guard = 0x59A4E2B180C3D7F6 | ASLR ON.");
    dbg_log("[DMESG] SYSCALL: 48 system calls registered in ring-0 jump table.");
}

static void debugger_on_open(window_t *w) { debugger_on_init(w); }

static void debugger_on_key(window_t *w, int c, rtc_time_t *t, uint32_t frame_cnt) {
    (void)w; (void)t; (void)frame_cnt;
    if (s_show_bsod) {
        if (c == 'r' || c == 'R' || c == 27) {
            s_show_bsod = false;
            dbg_log("[DEBUG] Recovered from simulated Blue Screen of Death.");
        }
        return;
    }

    if (c == 'b' || c == 'B') {
        s_show_bsod = true;
    } else if (c == 's' || c == 'S') {
        dbg_log(">> Symbol Lookup: 0xC0104280 -> kmain() + 0x42");
        dbg_log(">> Symbol Lookup: 0xC0108A10 -> scheduler_tick() + 0x18");
    } else if (c == 'm' || c == 'M') {
        dbg_log(">> Memory Dump @ 0xC0100000 (KERNEL_TEXT):");
        dbg_log("   55 48 89 E5 48 83 EC 20 89 7D FC 48 8B 05 ...");
    } else if (c == 'c' || c == 'C') {
        s_dbg_idx = 0;
    }
}

static void debugger_on_render(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink) {
    (void)t; (void)frame_cnt; (void)blink;
    if (!w) return;
    int bx = w->x;
    int by = w->y + TITLEBAR_H;

    if (s_show_bsod) {
        /* Full window Windows 10 style Blue Screen of Death simulation */
        draw_rect(bx, by, w->w, w->h - TITLEBAR_H, 0x000078D7);
        int ty = by + 30;
        draw_text(bx + 40, ty, ":(", 0x00FFFFFF, 0x000078D7);
        ty += 60;
        draw_text(bx + 40, ty, "Your AzamiOS PC ran into a problem and needs to restart.", 0x00FFFFFF, 0x000078D7);
        ty += 24;
        draw_text(bx + 40, ty, "We're just collecting some error info, and then we'll restart for you.", 0x00FFFFFF, 0x000078D7);
        ty += 50;
        draw_text(bx + 40, ty, "Stop Code: KERNEL_SECURITY_CHECK_FAILURE / STACK_CANARY_SMASH", 0x00FFFFFF, 0x000078D7);
        ty += 20;
        draw_text(bx + 40, ty, "Faulting Address: 0xDEADBEEF (in wm_core.c:on_render)", 0x00FFFFFF, 0x000078D7);
        ty += 50;
        draw_text(bx + 40, ty, "Press [R] or [ESC] to reboot / return to Desktop.", 0x00E2E8F0, 0x000078D7);
        return;
    }

    int px = bx + 10;
    int py = by + 10;

    draw_rect(px, py, w->w - 20, 80, 0x00E2E8F0);
    draw_text(px + 10, py + 8,  "AzamiOS Kernel Debugger & Profiler (Ring-0 Access)", COL_TEXT_BLUE, 0x00E2E8F0);
    draw_text(px + 10, py + 26, "Kernel Symbols : 1,428 Exported Symbols loaded in memory map.", COL_TEXT_DARK, 0x00E2E8F0);
    draw_text(px + 10, py + 44, "GDB Stub       : Listening on serial COM1 (port 1234)", COL_TEXT_DARK, 0x00E2E8F0);

    draw_text(px, py + 90, "Kernel Log Buffer (dmesg):", COL_TEXT_DARK, COL_WIN_BODY);
    draw_rect(px, py + 108, w->w - 20, 240, 0x000F172A);

    int y = py + 116;
    for (int i = 0; i < s_dbg_idx; i++) {
        uint32_t col = COL_TEXT_GREEN;
        if (strstr(s_dbg_log[i], "PANIC") || strstr(s_dbg_log[i], "BSOD")) col = COL_TEXT_RED;
        else if (strstr(s_dbg_log[i], "Lookup")) col = COL_TEXT_YELLOW;
        draw_text(px + 8, y, s_dbg_log[i], col, 0x000F172A);
        y += 18;
    }

    draw_text(px, py + 360, "Controls: [B] Trigger BSOD  |  [S] Symbol Lookup  |  [M] Memory Dump  |  [C] Clear", COL_TEXT_DARK, COL_WIN_BODY);
}

void debugger_service_init(void) {
    static const wm_service_t dbg_srv = {
        WIN_DEBUGGER, "Kernel Debugger", 0,
        debugger_on_init, debugger_on_open, NULL, debugger_on_render, debugger_on_key
    };
    wm_register_service(&dbg_srv);
}
