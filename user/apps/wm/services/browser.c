/**
 * browser.c — Text-Mode / Lynx-style Web Browser Window Service for AzamiOS v6.0
 * Supports URL navigation, bookmarks, links [1][2], scrolling, and simulated live/cached pages.
 */
#include "../wm.h"

#define BROWSER_COLS 76
#define BROWSER_ROWS 28

static char s_url[64] = "http://azami.org";
static char s_page_lines[BROWSER_ROWS * 3][BROWSER_COLS + 1];
static int  s_total_lines = 0;
static int  s_scroll_y = 0;
static bool s_url_focus = false;
static int  s_url_len = 16;

static void load_page(const char *url) {
    s_total_lines = 0;
    s_scroll_y = 0;
    wm_strlcpy(s_url, url, 64);
    s_url_len = strlen(s_url);

    if (strcmp(url, "http://azami.org") == 0 || strcmp(url, "azami.org") == 0) {
        wm_strlcpy(s_page_lines[s_total_lines++], "========================================================================", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "                     AZAMIOS OFFICIAL HOME PAGE                         ", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "========================================================================", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "Welcome to AzamiOS v6.0 — The Ultra-Fast Modern Microkernel OS!", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "Built with precision: Bochs VBE + Intel i915 GPU, Windows 10 DE, & Games.", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "Featured Portal Links:", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "  [1] Google Search Gateway (http://google.com)", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "  [2] Wikipedia Free Encyclopedia (http://wikipedia.org)", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "  [3] AzamiOS Kernel Docs & Git Repository (http://git.azami.org)", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "Latest News (July 2026):", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], " * AzamiOS v6.0 released with full Win10 Aero Snap & Fluent themes.", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], " * New built-in MicroPython REPL and 11 GNU core utilities.", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], " * Kernel panic debugging tools: BSOD generator & symbol resolution.", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "(Tip: Press numbers 1-3 to navigate or press TAB/ESC to edit URL)", BROWSER_COLS);
    } else if (strcmp(url, "http://google.com") == 0 || strcmp(url, "google.com") == 0) {
        wm_strlcpy(s_page_lines[s_total_lines++], "------------------------------------------------------------------------", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "                             GOOGLE SEARCH                              ", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "------------------------------------------------------------------------", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "Query box: [                            ] (Type query & hit Enter)", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "Trending Searches:", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "  [1] Back to AzamiOS Portal (http://azami.org)", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "  [2] How to build x86_64 microkernels in C", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "  [3] Intel i915 Graphics Driver Register Reference", BROWSER_COLS);
    } else if (strcmp(url, "http://wikipedia.org") == 0 || strcmp(url, "wikipedia.org") == 0) {
        wm_strlcpy(s_page_lines[s_total_lines++], "========================================================================", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "                       WIKIPEDIA — THE FREE ENCYCLOPEDIA                ", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "========================================================================", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "Article of the Day: Microkernel Architecture", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "In computer science, a microkernel is the near-minimum amount of", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "software that can provide the mechanisms needed to implement an OS.", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "These mechanisms include low-level address space management, thread", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "management, and inter-process communication (IPC).", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "Navigation Links:", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "  [1] Home Portal (http://azami.org)", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "  [2] Google Search (http://google.com)", BROWSER_COLS);
    } else {
        wm_strlcpy(s_page_lines[s_total_lines++], "HTTP 404 / DNS Notice — Host unreachable or page not found.", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "Requested URL:", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], url, BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "Try navigating to:", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "  [1] http://azami.org", BROWSER_COLS);
        wm_strlcpy(s_page_lines[s_total_lines++], "  [2] http://google.com", BROWSER_COLS);
    }
}

static void browser_on_init(window_t *w) {
    if (!w) return;
    wm_resize_window(w, 760, 540);
    load_page("http://azami.org");
    s_url_focus = false;
}

static void browser_on_open(window_t *w) { browser_on_init(w); }

static void browser_on_key(window_t *w, int c, rtc_time_t *t, uint32_t frame_cnt) {
    (void)w; (void)t; (void)frame_cnt;
    if (c == '\t' || c == 27) {
        s_url_focus = !s_url_focus;
        return;
    }
    if (s_url_focus) {
        if (c == '\r' || c == '\n') {
            s_url_focus = false;
            load_page(s_url);
        } else if (c == '\b' || c == 127) {
            if (s_url_len > 0) s_url[--s_url_len] = '\0';
        } else if (c >= 32 && c <= 126 && s_url_len < 63) {
            s_url[s_url_len++] = (char)c;
            s_url[s_url_len] = '\0';
        }
        return;
    }

    if (c == 'w' || c == 'W') { if (s_scroll_y > 0) s_scroll_y--; }
    else if (c == 's' || c == 'S') { if (s_scroll_y < s_total_lines - BROWSER_ROWS) s_scroll_y++; }
    else if (c == '1') {
        if (strstr(s_url, "azami.org") != NULL) load_page("http://google.com");
        else load_page("http://azami.org");
    } else if (c == '2') {
        if (strstr(s_url, "azami.org") != NULL) load_page("http://wikipedia.org");
        else if (strstr(s_url, "google.com") != NULL) load_page("http://azami.org");
        else load_page("http://google.com");
    } else if (c == '3') {
        load_page("http://azami.org");
    }
}

static void browser_on_render(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink) {
    (void)t; (void)frame_cnt;
    if (!w) return;
    int bx = w->x + 8;
    int by = w->y + TITLEBAR_H + 8;

    /* URL Bar */
    draw_rect(bx, by, w->w - 16, 26, s_url_focus ? 0x00E2E8F0 : 0x00CBD5E1);
    draw_rect(bx + 1, by + 1, w->w - 18, 24, 0x00F8FAFC);

    char ubar[80];
    snprintf(ubar, sizeof(ubar), "URL: %s%s", s_url, (s_url_focus && blink < 20) ? "_" : "");
    draw_text(bx + 8, by + 6, ubar, COL_TEXT_DARK, 0x00F8FAFC);

    /* Page Viewport */
    int py = by + 32;
    draw_rect(bx, py, w->w - 16, w->h - TITLEBAR_H - 48, 0x000F172A);

    int y = py + 8;
    for (int i = 0; i < BROWSER_ROWS && (s_scroll_y + i) < s_total_lines; i++) {
        const char *line = s_page_lines[s_scroll_y + i];
        uint32_t col = COL_TEXT_WHITE;
        if (strstr(line, "http://") || strstr(line, "[1]") || strstr(line, "[2]") || strstr(line, "[3]")) {
            col = COL_TEXT_CYAN;
        } else if (line[0] == '=' || line[0] == '-') {
            col = COL_TEXT_YELLOW;
        }
        draw_text(bx + 8, y, line, col, 0x000F172A);
        y += 16;
    }

    draw_text(bx, py + w->h - TITLEBAR_H - 62, "[TAB]: Edit URL | [1-3]: Open Link | [W/S]: Scroll Page", COL_TEXT_DARK, COL_WIN_BODY);
}

void browser_service_init(void) {
    static const wm_service_t browser_srv = {
        WIN_BROWSER, "Lynx Browser", 0,
        browser_on_init, browser_on_open, NULL, browser_on_render, browser_on_key
    };
    wm_register_service(&browser_srv);
}
