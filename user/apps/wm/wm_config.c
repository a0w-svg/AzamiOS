/**
 * wm_config.c — AzamiOS WM Filesystem Configuration Manager v5.0
 *
 * Responsibilities:
 *   1. Theme files (*.theme, key=value) — loaded from filesystem at startup.
 *      Default files are written on first run if missing.
 *   2. wm.conf — stores active DE index and active theme filename.
 *      Read at startup; written whenever DE or theme changes.
 *   3. wm_state — binary full-desktop-state snapshot.
 *      Written by terminal.c before exec(); read back at startup to restore
 *      all window positions/open-states seamlessly after child process exits.
 *
 * Theme file format (plain key=value, colours as 6-char RRGGBB hex):
 *   name=Ocean Dark
 *   wall_top=070D1A
 *   accent=2563EB
 *   font=default
 *   …
 *
 * wm.conf format:
 *   de=0
 *   theme=ocean.theme
 */

#include "wm.h"

/* ── Global theme storage (defined here, declared extern in wm.h) ─────────── */
wm_theme_t g_themes[MAX_THEMES];
char       g_theme_filenames[MAX_THEMES][32];
int        g_num_themes = 0;

/* Active config indices (used by wm_config_get_*) */
static int  g_conf_de_id      = WM_DE_WIN10;
static int  g_conf_theme_idx  = 0;

/* ── Full-WM-state binary struct ─────────────────────────────────────────── */
#define WM_STATE_MAGIC 0x574D5354U  /* "WMST" */
#define WM_STATE_FILE  "wm_state"

typedef struct {
    uint32_t magic;
    int32_t  de_id;
    int32_t  theme_idx;
    int32_t  focus;
    struct {
        uint8_t  open;
        uint8_t  minimized;
        uint8_t  maximized;
        uint8_t  _pad;
        int32_t  x, y, w, h;
    } wins[MAX_WINS];
} wm_full_state_t;

/* ── Low-level IO helpers ────────────────────────────────────────────────── */
static void io_write_str(int fd, const char *s) {
    int len = 0;
    const char *p = s;
    while (*p++) len++;
    write(fd, s, len);
}

static void io_write_char(int fd, char c) {
    write(fd, &c, 1);
}

/* Read one text line from fd (strips \r\n). Returns byte-count, -1 on EOF. */
static int io_read_line(int fd, char *buf, int maxlen) {
    int  n = 0;
    char c;
    while (n < maxlen - 1) {
        int r = read(fd, &c, 1);
        if (r <= 0) { if (n == 0) return -1; break; }
        if (c == '\n') break;
        if (c != '\r') buf[n++] = c;
    }
    buf[n] = '\0';
    return n;
}

/* ── Colour helpers ──────────────────────────────────────────────────────── */
/* Parse up to 6 hex chars from s → 0x00RRGGBB */
static uint32_t parse_hex_col(const char *s) {
    uint32_t v = 0;
    for (int i = 0; i < 6 && s[i]; i++) {
        char c = s[i];
        v <<= 4;
        if      (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
    }
    return v;
}

/* Write 6 uppercase hex digits of colour (without 0x prefix) */
static void write_hex_col(int fd, uint32_t col) {
    static const char h[] = "0123456789ABCDEF";
    col &= 0x00FFFFFFU;
    char b[7];
    b[0] = h[(col >> 20) & 0xF]; b[1] = h[(col >> 16) & 0xF];
    b[2] = h[(col >> 12) & 0xF]; b[3] = h[(col >>  8) & 0xF];
    b[4] = h[(col >>  4) & 0xF]; b[5] = h[(col >>  0) & 0xF];
    b[6] = '\0';
    write(fd, b, 6);
}

/* ── Theme file write ────────────────────────────────────────────────────── */
static void write_theme_file(const char *fname,
    const char *name,
    uint32_t wall_top, uint32_t wall_bot,
    uint32_t taskbar, uint32_t taskbar_line, uint32_t start_btn,
    uint32_t title_active, uint32_t title_inactive,
    uint32_t win_frame, uint32_t win_body,
    uint32_t accent, uint32_t accent2,
    uint32_t text_primary, uint32_t text_secondary,
    const char *font)
{
    int fd = open(fname, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (fd < 0) return;

#define WL(k,v) io_write_str(fd,k"="); io_write_str(fd,v); io_write_char(fd,'\n');
#define WC(k,c) io_write_str(fd,k"="); write_hex_col(fd,c); io_write_char(fd,'\n');

    WL("name",           name)
    WC("wall_top",       wall_top)
    WC("wall_bot",       wall_bot)
    WC("taskbar",        taskbar)
    WC("taskbar_line",   taskbar_line)
    WC("start_btn",      start_btn)
    WC("title_active",   title_active)
    WC("title_inactive", title_inactive)
    WC("win_frame",      win_frame)
    WC("win_body",       win_body)
    WC("accent",         accent)
    WC("accent2",        accent2)
    WC("text_primary",   text_primary)
    WC("text_secondary", text_secondary)
    WL("font",           font)

#undef WL
#undef WC
    close(fd);
}

/* ── Create default theme files if missing ───────────────────────────────── */
static void ensure_default_themes(void) {
    int fd;

    fd = open("ocean.theme", 0);
    if (fd < 0) {
        write_theme_file("ocean.theme", "Ocean Dark",
            0x00070D1A, 0x00162544,
            0x001A2332, 0x002563EB, 0x002563EB,
            0x001D4ED8, 0x0064748B,
            0x00F0F4F8, 0x00FFFFFF,
            0x002563EB, 0x0038BDF8,
            0x00FFFFFF, 0x0094A3B8, "default");
    } else { close(fd); }

    fd = open("amber.theme", 0);
    if (fd < 0) {
        write_theme_file("amber.theme", "Amber Warm",
            0x001A0F00, 0x00402200,
            0x00291400, 0x00F59E0B, 0x00D97706,
            0x00B45309, 0x00786030,
            0x00FEF3C7, 0x00FFFBF0,
            0x00F59E0B, 0x00FCD34D,
            0x00FFFFFF, 0x00D1A054, "default");
    } else { close(fd); }

    fd = open("cyber.theme", 0);
    if (fd < 0) {
        write_theme_file("cyber.theme", "Cyber Purple",
            0x000D0015, 0x00200030,
            0x0014001F, 0x00A855F7, 0x007C3AED,
            0x006D28D9, 0x00581C87,
            0x00F5F3FF, 0x00FEFCFF,
            0x00A855F7, 0x00E879F9,
            0x00FFFFFF, 0x00C084FC, "default");
    } else { close(fd); }

    fd = open("fluent.theme", 0);
    if (fd < 0) {
        write_theme_file("fluent.theme", "Fluent Blue",
            0x000F172A, 0x001E3A8A,
            0x000F172A, 0x003B82F6, 0x003B82F6,
            0x001E293B, 0x00475569,
            0x00F8FAFC, 0x00FFFFFF,
            0x002563EB, 0x0060A5FA,
            0x000F172A, 0x0064748B, "default");
    } else { close(fd); }

    fd = open("nord.theme", 0);
    if (fd < 0) {
        write_theme_file("nord.theme", "Nord Arctic",
            0x002E3440, 0x003B4252,
            0x002E3440, 0x0088C0D0, 0x0081A1C1,
            0x00434C5E, 0x004C566A,
            0x00ECEFF4, 0x00E5E9F0,
            0x0088C0D0, 0x008FBCBB,
            0x002E3440, 0x004C566A, "default");
    } else { close(fd); }
}

/* ── Parse and load one theme file into g_themes[idx] ───────────────────── */
static bool load_one_theme(const char *fname, int idx) {
    int fd = open(fname, 0);
    if (fd < 0) return false;

    wm_theme_t *th = &g_themes[idx];
    th->id = idx;
    wm_strlcpy(th->name, fname, sizeof(th->name));  /* fallback name */
    wm_strlcpy(th->font, "default", sizeof(th->font));
    /* fallback colours — Ocean Dark defaults */
    th->wall_top       = 0x00070D1A; th->wall_bot       = 0x00162544;
    th->taskbar        = 0x001A2332; th->taskbar_line   = 0x002563EB;
    th->start_btn      = 0x002563EB; th->title_active   = 0x001D4ED8;
    th->title_inactive = 0x0064748B; th->win_frame      = 0x00F0F4F8;
    th->win_body       = 0x00FFFFFF; th->accent         = 0x002563EB;
    th->accent2        = 0x0038BDF8; th->text_primary   = 0x00FFFFFF;
    th->text_secondary = 0x0094A3B8;

    char line[72];
    int  len;
    while ((len = io_read_line(fd, line, sizeof(line))) >= 0) {
        if (len == 0 || line[0] == '#') continue;
        /* find '=' separator */
        int eq = -1;
        for (int i = 0; line[i]; i++) { if (line[i] == '=') { eq = i; break; } }
        if (eq <= 0) continue;

        char key[24]; int kl = eq < 23 ? eq : 23;
        memcpy(key, line, (size_t)kl); key[kl] = '\0';
        const char *val = line + eq + 1;

#define MATCH_C(k, field) if (strcmp(key,k)==0) { th->field = parse_hex_col(val); continue; }
#define MATCH_S(k, field, sz) if (strcmp(key,k)==0) { wm_strlcpy(th->field, val, sz); continue; }

        MATCH_S("name",           name,           sizeof(th->name))
        MATCH_C("wall_top",       wall_top)
        MATCH_C("wall_bot",       wall_bot)
        MATCH_C("taskbar",        taskbar)
        MATCH_C("taskbar_line",   taskbar_line)
        MATCH_C("start_btn",      start_btn)
        MATCH_C("title_active",   title_active)
        MATCH_C("title_inactive", title_inactive)
        MATCH_C("win_frame",      win_frame)
        MATCH_C("win_body",       win_body)
        MATCH_C("accent",         accent)
        MATCH_C("accent2",        accent2)
        MATCH_C("text_primary",   text_primary)
        MATCH_C("text_secondary", text_secondary)
        MATCH_S("font",           font,           sizeof(th->font))

#undef MATCH_C
#undef MATCH_S
    }
    close(fd);
    return true;
}

/* ── Scan for *.theme files using opendir ───────────────────────────────── */
static void scan_theme_files(void) {
    g_num_themes = 0;

    /* Try the five built-in names first (order matters for keyboard shortcuts) */
    static const char *default_names[5] = { "fluent.theme", "nord.theme", "ocean.theme", "amber.theme", "cyber.theme" };
    for (int i = 0; i < 5; i++) {
        if (g_num_themes >= MAX_THEMES) break;
        if (load_one_theme(default_names[i], g_num_themes)) {
            wm_strlcpy(g_theme_filenames[g_num_themes], default_names[i], 32);
            g_num_themes++;
        }
    }

    /* Now scan the root directory for any additional *.theme files */
    DIR *d = opendir("/");
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && g_num_themes < MAX_THEMES) {
        const char *n = ent->d_name;
        /* Check if name ends in ".theme" and isn't one we already loaded */
        int nl = 0; for (const char *p = n; *p; p++) nl++;
        if (nl < 7) continue;
        if (strcmp(n + nl - 6, ".theme") != 0) continue;
        /* Skip if already loaded */
        bool dup = false;
        for (int j = 0; j < g_num_themes; j++) {
            if (strcmp(g_theme_filenames[j], n) == 0) { dup = true; break; }
        }
        if (dup) continue;
        if (load_one_theme(n, g_num_themes)) {
            wm_strlcpy(g_theme_filenames[g_num_themes], n, 32);
            g_num_themes++;
        }
    }
    closedir(d);

    /* Always guarantee at least one theme */
    if (g_num_themes == 0) {
        /* Synthesise a fallback Ocean Dark in slot 0 */
        wm_strlcpy(g_themes[0].name, "Ocean Dark", sizeof(g_themes[0].name));
        wm_strlcpy(g_themes[0].font, "default",    sizeof(g_themes[0].font));
        g_themes[0].id = 0;
        g_themes[0].wall_top       = 0x00070D1A; g_themes[0].wall_bot       = 0x00162544;
        g_themes[0].taskbar        = 0x001A2332; g_themes[0].taskbar_line   = 0x002563EB;
        g_themes[0].start_btn      = 0x002563EB; g_themes[0].title_active   = 0x001D4ED8;
        g_themes[0].title_inactive = 0x0064748B; g_themes[0].win_frame      = 0x00F0F4F8;
        g_themes[0].win_body       = 0x00FFFFFF; g_themes[0].accent         = 0x002563EB;
        g_themes[0].accent2        = 0x0038BDF8; g_themes[0].text_primary   = 0x00FFFFFF;
        g_themes[0].text_secondary = 0x0094A3B8;
        wm_strlcpy(g_theme_filenames[0], "ocean.theme", 32);
        g_num_themes = 1;
    }
}

/* ── wm.conf load/save ───────────────────────────────────────────────────── */
void wm_load_conf(void) {
    int fd = open("wm.conf", 0);
    if (fd < 0) return;

    char line[64];
    int  len;
    while ((len = io_read_line(fd, line, sizeof(line))) >= 0) {
        if (len == 0 || line[0] == '#') continue;
        int eq = -1;
        for (int i = 0; line[i]; i++) { if (line[i] == '=') { eq = i; break; } }
        if (eq <= 0) continue;
        char key[24]; int kl = eq < 23 ? eq : 23;
        memcpy(key, line, (size_t)kl); key[kl] = '\0';
        const char *val = line + eq + 1;

        if (strcmp(key, "de") == 0) {
            g_conf_de_id = 0;
            for (int i = 0; val[i] >= '0' && val[i] <= '9'; i++)
                g_conf_de_id = g_conf_de_id * 10 + (val[i] - '0');
        } else if (strcmp(key, "theme") == 0) {
            /* Find the index of this filename in g_theme_filenames */
            for (int i = 0; i < g_num_themes; i++) {
                if (strcmp(g_theme_filenames[i], val) == 0) {
                    g_conf_theme_idx = i;
                    break;
                }
            }
        }
    }
    close(fd);
}

void wm_save_conf(void) {
    int fd = open("wm.conf", O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (fd < 0) return;

    /* de */
    io_write_str(fd, "de=");
    char nbuf[12]; int n = 0;
    int v = (g_de ? g_de->id : 0);
    if (v == 0) { nbuf[n++] = '0'; }
    else { int tmp = v; while (tmp > 0) { nbuf[n++] = (char)('0' + tmp % 10); tmp /= 10; }
           /* reverse */ for (int i=0,j=n-1; i<j; i++,j--) { char t=nbuf[i]; nbuf[i]=nbuf[j]; nbuf[j]=t; } }
    nbuf[n] = '\0';
    io_write_str(fd, nbuf); io_write_char(fd, '\n');

    /* theme filename */
    io_write_str(fd, "theme=");
    const char *tfname = "cyber.theme";
    if (g_theme) {
        for (int i = 0; i < g_num_themes; i++) {
            if (g_theme == &g_themes[i]) { tfname = g_theme_filenames[i]; break; }
        }
    }
    io_write_str(fd, tfname); io_write_char(fd, '\n');

    close(fd);
}

/* ── DE/Theme internal setters (no conf write — used during state restore) ─── */
static void _wm_apply_de(int de_id) {
    wm_de_t *next = wm_get_de(de_id);
    if (next) g_de = next;
}

static void _wm_apply_theme(int theme_idx) {
    wm_theme_t *next = wm_get_theme(theme_idx);
    if (next) g_theme = next;
}

/* ── DE Switching (runtime, zero-restart) ──────────────────────────────────────── */
/**
 * wm_switch_de: Swaps the active Desktop Environment pointer.
 * Persists the new choice to wm.conf immediately.
 */
void wm_switch_de(int de_id) {
    _wm_apply_de(de_id);
    wm_save_conf();
}

/**
 * wm_switch_theme: Swaps the active color palette to g_themes[theme_idx].
 * Persists the new choice to wm.conf immediately.
 */
void wm_switch_theme(int theme_idx) {
    _wm_apply_theme(theme_idx);
    wm_save_conf();
}

/* ── Public init (call once at startup, before wm_switch_de/theme) ────────── */
void wm_config_init(void) {
    ensure_default_themes();
    scan_theme_files();
    wm_load_conf();          /* sets g_conf_de_id, g_conf_theme_idx */
}

int wm_config_get_de(void)         { return g_conf_de_id; }
int wm_config_get_theme_idx(void)  { return g_conf_theme_idx; }

wm_theme_t *wm_get_theme(int idx) {
    if (idx < 0 || idx >= g_num_themes) return &g_themes[0];
    return &g_themes[idx];
}

/* ── Full WM state: save before exec(), restore after WM restart ─────────── */

/**
 * wm_save_full_state: Snapshots all window open/position states plus the
 * active DE and theme index to a binary file (WM_STATE_FILE).
 * Called by terminal.c immediately before exec().
 */
void wm_save_full_state(void) {
    wm_full_state_t st;
    st.magic     = WM_STATE_MAGIC;
    st.de_id     = g_de    ? g_de->id : WM_DE_WIN10;
    st.theme_idx = 0;
    st.focus     = g_focus;

    if (g_theme) {
        for (int i = 0; i < g_num_themes; i++) {
            if (g_theme == &g_themes[i]) { st.theme_idx = i; break; }
        }
    }

    for (int i = 0; i < MAX_WINS; i++) {
        if (i < g_num_wins) {
            st.wins[i].open      = g_wins[i].open      ? 1 : 0;
            st.wins[i].minimized = g_wins[i].minimized ? 1 : 0;
            st.wins[i].maximized = g_wins[i].maximized ? 1 : 0;
            st.wins[i]._pad      = 0;
            st.wins[i].x = g_wins[i].x; st.wins[i].y = g_wins[i].y;
            st.wins[i].w = g_wins[i].w; st.wins[i].h = g_wins[i].h;
        } else {
            st.wins[i].open = 0;
        }
    }

    int fd = open(WM_STATE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (fd >= 0) {
        write(fd, &st, sizeof(st));
        close(fd);
    }
}

/**
 * wm_load_full_state: Reads the saved snapshot and restores window states,
 * DE, and theme. Called at startup, AFTER init_wins() sets defaults.
 * Returns true if state was successfully restored; false if no snapshot exists.
 */
bool wm_load_full_state(void) {
    int fd = open(WM_STATE_FILE, 0);
    if (fd < 0) return false;

    wm_full_state_t st;
    int n = read(fd, &st, sizeof(st));
    close(fd);

    if (n != (int)sizeof(st) || st.magic != WM_STATE_MAGIC) return false;

    /* Restore DE and theme — use internal setters to avoid triggering
     * two unnecessary wm_save_conf() disk writes at startup. */
    _wm_apply_de(st.de_id);
    _wm_apply_theme(st.theme_idx);

    /* Restore window states (positions, open flags) */
    for (int i = 0; i < g_num_wins && i < MAX_WINS; i++) {
        g_wins[i].open      = (st.wins[i].open      != 0);
        g_wins[i].minimized = (st.wins[i].minimized != 0);
        g_wins[i].maximized = (st.wins[i].maximized != 0);
        g_wins[i].x = st.wins[i].x; g_wins[i].y = st.wins[i].y;
        /* Validate and clamp restored dimensions to safe ranges */
        g_wins[i].w = st.wins[i].w; g_wins[i].h = st.wins[i].h;
        if (g_wins[i].w < 64)  g_wins[i].w = 64;
        if (g_wins[i].h < 40)  g_wins[i].h = 40;
        if (g_wins[i].w > SCREEN_W) g_wins[i].w = SCREEN_W;
        if (g_wins[i].h > DESKTOP_H) g_wins[i].h = DESKTOP_H;
        /* Re-clamp position to keep the window on-screen */
        if (g_wins[i].x < 0) g_wins[i].x = 0;
        if (g_wins[i].y < 0) g_wins[i].y = 0;
        if (g_wins[i].x + g_wins[i].w > SCREEN_W) g_wins[i].x = SCREEN_W - g_wins[i].w;
        if (g_wins[i].y + g_wins[i].h > DESKTOP_H) g_wins[i].y = DESKTOP_H - g_wins[i].h;
    }
    if (st.focus >= 0 && st.focus < g_num_wins) g_focus = st.focus;

    /* If restore resulted in no open windows (edge case), open Welcome by default */
    bool any_open = false;
    for (int i = 0; i < g_num_wins; i++) if (g_wins[i].open) { any_open = true; break; }
    if (!any_open) {
        int idx = find_win_by_type(WIN_WELCOME);
        if (idx >= 0) { g_wins[idx].open = true; g_focus = idx; }
    }

    return true;
}

/**
 * wm_delete_full_state: Truncates the state snapshot file to zero after a
 * successful restore (or on corruption). Uses O_TRUNC so the kernel actually
 * clears the content — a plain open-then-close without O_TRUNC would leave
 * stale bytes in the file and cause false-positive restores on the next boot.
 */
void wm_delete_full_state(void) {
    int fd = open(WM_STATE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (fd >= 0) close(fd);
}
