/* ============================================================================
 * AzamiOS Desktop Environment — Modern File Manager (v7.0)
 * File: userland/apps/filemanager/main.c
 *
 * Features:
 *  • Dual-pane file manager: Sidebar Places & Main File Browser
 *  • POSIX Access Control List (ACL) inspection & permissions column ("rwxr-xr-x+")
 *  • File action toolbar: [Open], [Edit], [Terminal], [New Folder], [New Note],
 *    [Rename], [Delete]
 *  • Properties & ACL inspector popup dialog
 *  • Clickable Directory Breadcrumbs — each path segment jumps straight there
 *  • Right-click context menu (Open / Rename / Delete / Properties / New…)
 *  • Inline rename & "new folder" prompt, with a confirmation step on delete
 *  • Mouse-wheel scrolling with a live scrollbar thumb
 *  • Folders-first, alphabetical directory sort
 *  • Type-ahead: press a letter to jump to the next matching entry
 *  • Rich file type detection (.elf, .txt, .c, .h, .conf, .sh, .img, .ext2)
 *  • Full keyboard navigation (Arrows, Home/End, Enter, Backspace, Del, F2)
 * ============================================================================ */

#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/ctype.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/dirent.h"
#include "../../libc/include/sys/stat.h"
#include "../../libc/include/sys/statvfs.h"
#include "../../libc/include/sys/syscall.h"
#include "../../libc/include/sys/acl.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/ui_kit.h"

#define SERVER_CHAN  1
#define WIN_W       740
#define WIN_H       500
#define MAP_ADDR    ((void *)0x66000000)

#define SIDEBAR_W   150
#define LIST_OX     (SIDEBAR_W + 10)
#define LIST_OY     78
#define ROW_H       24
#define VISIBLE_ROWS 15
#define SCROLLBAR_X (WIN_W - 16)

static uk_window_t g_win;
static int g_selected = -1;
static int g_hovered  = -1;
static char g_current_path[256] = "/";
static int g_scroll = 0;

static bool g_show_props = false;
static char g_props_text[512] = "";

/* ── Breadcrumb hit-testing ────────────────────────────────────────────────
 * Populated each time the breadcrumb bar is drawn so clicks can map back
 * onto the ancestor directory a given segment represents. */
#define MAX_CRUMBS 16
typedef struct {
    int x0, x1;
    char path[256];
} crumb_t;
static crumb_t g_crumbs[MAX_CRUMBS];
static int g_crumb_count = 0;

/* ── Modal prompt (rename / new folder) ───────────────────────────────────── */
typedef enum { PROMPT_NONE = 0, PROMPT_RENAME, PROMPT_NEW_FOLDER } prompt_mode_t;
static prompt_mode_t g_prompt_mode = PROMPT_NONE;
static char g_prompt_buf[64] = "";
static int g_prompt_len = 0;

/* ── Delete confirmation ───────────────────────────────────────────────────── */
static bool g_confirm_delete = false;

/* ── Modal entrance animation (driven by the 100ms AZ_WM_TIMER_TICK below) ──
 * Floating modals (new-folder prompt, delete confirmation, properties) rise
 * into place from just below their resting position rather than snapping in
 * — the same ease-out-quad motion azwm itself uses to open windows, applied
 * to a plain vertical offset so nothing inside the modal has to reflow. */
#define MODAL_ANIM_STEPS 4     /* 4 * 100ms = 400ms rise            */
#define MODAL_SLIDE_PX   24    /* starts this far below its resting Y */
static int g_modal_anim_step = MODAL_ANIM_STEPS; /* settled by default */

static void start_modal_anim(void) { g_modal_anim_step = 0; }

/* Current vertical offset (in px, counting down to 0) to add to a modal's
 * resting Y position — call once per modal per frame. */
static int modal_anim_dy(void)
{
    int ease = uk_ease_out_quad(g_modal_anim_step * 256 / MODAL_ANIM_STEPS);
    return uk_lerp(MODAL_SLIDE_PX, 0, ease);
}

/* ── Right-click context menu ─────────────────────────────────────────────── */
static bool g_ctx_open = false;
static int g_ctx_x = 0, g_ctx_y = 0;
static int g_ctx_hover = -1;
static int g_ctx_target = -1; /* file index the menu was opened on, or -1 */

static const char *g_ctx_items_file[] = {
    "Open", "Edit", "Copy Path", "Open Terminal", "Rename", "Properties", "Delete",
};
#define NUM_CTX_FILE ((int)(sizeof(g_ctx_items_file)/sizeof(g_ctx_items_file[0])))

static const char *g_ctx_items_blank[] = {
    "New Folder", "New Note", "Open Terminal", "Refresh",
};
#define NUM_CTX_BLANK ((int)(sizeof(g_ctx_items_blank)/sizeof(g_ctx_items_blank[0])))

/* ── Toolbar (data-driven so hit-testing can't drift from drawing) ───────── */
typedef enum {
    ACT_OPEN, ACT_EDIT, ACT_TERMINAL, ACT_NEW_FOLDER,
    ACT_NEW_NOTE, ACT_RENAME, ACT_DELETE,
} toolbar_action_t;

typedef struct {
    const char *label;
    int width;
    toolbar_action_t action;
} toolbar_btn_t;

static const toolbar_btn_t g_toolbar[] = {
    { "Open",       60, ACT_OPEN },
    { "Edit",       60, ACT_EDIT },
    { "Terminal",   70, ACT_TERMINAL },
    { "New Folder", 85, ACT_NEW_FOLDER },
    { "New Note",   75, ACT_NEW_NOTE },
    { "Rename",     65, ACT_RENAME },
    { "Delete",     60, ACT_DELETE },
};
#define NUM_TOOLBAR ((int)(sizeof(g_toolbar)/sizeof(g_toolbar[0])))
#define TOOLBAR_GAP 8
#define TOOLBAR_X0  12

/* ── Places / Sidebar bookmarks ───────────────────────────────────────────
 * Loaded from /etc/filemanager_places.conf ("label|path|icon" per line,
 * '#' comments and blank lines skipped) so the sidebar can be customised
 * without recompiling; load_default_places() covers a missing or
 * unreadable file with the same list this used to have hardcoded. */
typedef struct {
    char label[24];
    char path[128];
    char icon[4];
} place_item_t;

#define MAX_PLACES 16
static place_item_t g_places[MAX_PLACES];
static int g_num_places = 0;
static int g_selected_place = 0;

static void load_default_places(void)
{
    static const struct { const char *label, *path, *icon; } defaults[] = {
        { "Root",      "/",                   "/" },
        { "Desktop",   "/home/azami/Desktop", "D" },
        { "Hard Disk", "/hdd",                "H" },
        { "Binaries",  "/bin",                "B" },
        { "System",    "/sbin",               "S" },
        { "Config",    "/etc",                "E" },
        { "Proc FS",   "/proc",               "P" },
        { "Devices",   "/dev",                "D" },
        { "Temporary", "/tmp",                "T" },
    };
    g_num_places = 0;
    for (unsigned int i = 0; i < sizeof(defaults) / sizeof(defaults[0]) && g_num_places < MAX_PLACES; i++) {
        place_item_t *p = &g_places[g_num_places++];
        strncpy(p->label, defaults[i].label, sizeof(p->label) - 1);
        p->label[sizeof(p->label) - 1] = '\0';
        strncpy(p->path, defaults[i].path, sizeof(p->path) - 1);
        p->path[sizeof(p->path) - 1] = '\0';
        strncpy(p->icon, defaults[i].icon, sizeof(p->icon) - 1);
        p->icon[sizeof(p->icon) - 1] = '\0';
    }
}

static void load_places_config(void)
{
    int fd = sys_open("/etc/filemanager_places.conf", 0, 0);
    if (fd < 0) { load_default_places(); return; }

    static char buf[2048];
    int n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) { load_default_places(); return; }
    buf[n] = '\0';

    g_num_places = 0;
    char *saveptr = NULL;
    char *line = strtok_r(buf, "\n", &saveptr);
    while (line && g_num_places < MAX_PLACES) {
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '\0' || *line == '#') { line = strtok_r(NULL, "\n", &saveptr); continue; }

        char *fsave = NULL;
        char *label_s = strtok_r(line,  "|", &fsave);
        char *path_s  = strtok_r(NULL, "|", &fsave);
        char *icon_s  = strtok_r(NULL, "|", &fsave);
        if (label_s && path_s && icon_s) {
            place_item_t *p = &g_places[g_num_places++];
            strncpy(p->label, label_s, sizeof(p->label) - 1);
            p->label[sizeof(p->label) - 1] = '\0';
            strncpy(p->path, path_s, sizeof(p->path) - 1);
            p->path[sizeof(p->path) - 1] = '\0';
            strncpy(p->icon, icon_s, sizeof(p->icon) - 1);
            p->icon[sizeof(p->icon) - 1] = '\0';
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    if (g_num_places == 0) load_default_places();
}

/* Dynamically loaded filesystem entries */
typedef struct {
    char name[64];
    char size[24];
    char type[24];
    char mode[16];
    int  is_dir;
    int  has_acl;
    uid_t uid;
    gid_t gid;
} file_entry_t;

#define MAX_ENTRIES 128
static file_entry_t g_files[MAX_ENTRIES];
static int NFILES = 0;

/* True if `name` ends in "." + ext, case-insensitively (so "photo.JPG"
 * matches ext "jpg"). A plain strstr(name, ".ext") — the old approach —
 * matches substrings anywhere, so "readme.html" reads as ".h" and
 * "notes.txtbak" reads as ".txt"; this checks the actual suffix instead. */
static bool file_has_ext(const char *name, const char *ext)
{
    size_t nlen = strlen(name);
    size_t elen = strlen(ext);
    if (nlen < elen + 1) return false;
    const char *suffix = name + (nlen - elen);
    if (suffix[-1] != '.') return false;
    return strcasecmp(suffix, ext) == 0;
}

/* ── Extension → GUI app associations ─────────────────────────────────────
 * Loaded from /etc/mime.conf ("ext=/path/to/app.elf" per line, '#' comments
 * and blank lines skipped, first match for a given extension wins) so file
 * associations can be edited without recompiling; load_default_file_assoc()
 * covers a missing or unreadable file with the same list this used to have
 * hardcoded. */
typedef struct {
    char ext[8];
    char app_path[64];
} file_assoc_t;

#define MAX_FILE_ASSOC 32
static file_assoc_t g_file_assoc[MAX_FILE_ASSOC];
static int g_num_file_assoc = 0;

static void load_default_file_assoc(void)
{
    static const struct { const char *ext, *app_path; } defaults[] = {
        { "txt",  "/bin/texteditor.elf" },
        { "md",   "/bin/texteditor.elf" },
        { "c",    "/bin/texteditor.elf" },
        { "h",    "/bin/texteditor.elf" },
        { "conf", "/bin/texteditor.elf" },
        { "cfg",  "/bin/texteditor.elf" },
        { "ini",  "/bin/texteditor.elf" },
        { "log",  "/bin/texteditor.elf" },
        { "sh",   "/bin/texteditor.elf" },
        { "json", "/bin/texteditor.elf" },
        { "bmp",  "/bin/paint.elf" },
        { "png",  "/bin/paint.elf" },
        { "wav",  "/bin/audioplayer.elf" },
    };
    g_num_file_assoc = 0;
    for (unsigned int i = 0; i < sizeof(defaults) / sizeof(defaults[0]) && g_num_file_assoc < MAX_FILE_ASSOC; i++) {
        file_assoc_t *a = &g_file_assoc[g_num_file_assoc++];
        strncpy(a->ext, defaults[i].ext, sizeof(a->ext) - 1);
        a->ext[sizeof(a->ext) - 1] = '\0';
        strncpy(a->app_path, defaults[i].app_path, sizeof(a->app_path) - 1);
        a->app_path[sizeof(a->app_path) - 1] = '\0';
    }
}

static void load_file_assoc_config(void)
{
    int fd = sys_open("/etc/mime.conf", 0, 0);
    if (fd < 0) { load_default_file_assoc(); return; }

    static char buf[2048];
    int n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) { load_default_file_assoc(); return; }
    buf[n] = '\0';

    g_num_file_assoc = 0;
    char *saveptr = NULL;
    char *line = strtok_r(buf, "\n", &saveptr);
    while (line && g_num_file_assoc < MAX_FILE_ASSOC) {
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '\0' || *line == '#') { line = strtok_r(NULL, "\n", &saveptr); continue; }

        char *fsave = NULL;
        char *ext_s  = strtok_r(line,  "=", &fsave);
        char *path_s = strtok_r(NULL, "=", &fsave);
        if (ext_s && path_s) {
            file_assoc_t *a = &g_file_assoc[g_num_file_assoc++];
            strncpy(a->ext, ext_s, sizeof(a->ext) - 1);
            a->ext[sizeof(a->ext) - 1] = '\0';
            strncpy(a->app_path, path_s, sizeof(a->app_path) - 1);
            a->app_path[sizeof(a->app_path) - 1] = '\0';
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    if (g_num_file_assoc == 0) load_default_file_assoc();
}

/* Picks the GUI app best suited to open `name`, based on its extension.
 * Falls back to the text editor, a reasonable default on a system where
 * most non-executable files are small text/config files. */
static const char *resolve_app_for_file(const char *name)
{
    for (int i = 0; i < g_num_file_assoc; i++) {
        if (file_has_ext(name, g_file_assoc[i].ext))
            return g_file_assoc[i].app_path;
    }
    return "/bin/texteditor.elf";
}

static void build_full_path(char *out, size_t out_len, const char *name)
{
    if (strcmp(g_current_path, "/") == 0)
        snprintf(out, out_len, "/%s", name);
    else
        snprintf(out, out_len, "%s/%s", g_current_path, name);
}

static void format_file_size(size_t size, char *out, size_t out_len)
{
    if (size >= 1024 * 1024 * 1024) {
        unsigned int gib = (unsigned int)(size / (1024 * 1024 * 1024));
        unsigned int rem = (unsigned int)(((size % (1024 * 1024 * 1024)) * 10) / (1024 * 1024 * 1024));
        snprintf(out, out_len, "%u.%u GB", gib, rem);
    } else if (size >= 1024 * 1024) {
        unsigned int mib = (unsigned int)(size / (1024 * 1024));
        unsigned int rem = (unsigned int)(((size % (1024 * 1024)) * 10) / (1024 * 1024));
        snprintf(out, out_len, "%u.%u MB", mib, rem);
    } else if (size >= 1024) {
        unsigned int kib = (unsigned int)(size / 1024);
        unsigned int rem = (unsigned int)(((size % 1024) * 10) / 1024);
        snprintf(out, out_len, "%u.%u KB", kib, rem);
    } else {
        snprintf(out, out_len, "%u B", (unsigned int)size);
    }
}

/* Folders first, then alphabetical (case-insensitive) — the ".." entry (if
 * present) is always g_files[0] and is excluded from the sort below. */
static int file_entry_cmp(const void *a, const void *b)
{
    const file_entry_t *fa = (const file_entry_t *)a;
    const file_entry_t *fb = (const file_entry_t *)b;
    if (fa->is_dir != fb->is_dir) return fb->is_dir - fa->is_dir;
    return strcasecmp(fa->name, fb->name);
}

static void load_directory(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) return;

    strncpy(g_current_path, path, sizeof(g_current_path) - 1);
    g_current_path[sizeof(g_current_path) - 1] = '\0';

    NFILES = 0;
    g_selected = -1;
    g_hovered = -1;
    g_scroll = 0;
    g_show_props = false;
    g_ctx_open = false;
    g_confirm_delete = false;
    g_prompt_mode = PROMPT_NONE;

    /* Update selected place if matching */
    for (int p = 0; p < g_num_places; p++) {
        if (strcmp(g_current_path, g_places[p].path) == 0) {
            g_selected_place = p;
            break;
        }
    }

    /* If not in root, add parent directory ".." */
    int dotdot = 0;
    if (strcmp(g_current_path, "/") != 0) {
        strcpy(g_files[NFILES].name, "..");
        strcpy(g_files[NFILES].size, "-");
        strcpy(g_files[NFILES].type, "Folder");
        strcpy(g_files[NFILES].mode, "drwxr-xr-x");
        g_files[NFILES].is_dir = 1;
        g_files[NFILES].has_acl = 0;
        NFILES++;
        dotdot = 1;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && NFILES < MAX_ENTRIES) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        strncpy(g_files[NFILES].name, ent->d_name, sizeof(g_files[NFILES].name) - 1);
        g_files[NFILES].name[sizeof(g_files[NFILES].name) - 1] = '\0';

        char full_path[512];
        build_full_path(full_path, sizeof(full_path), ent->d_name);

        struct stat st;
        if (stat(full_path, &st) == 0) {
            g_files[NFILES].uid = st.st_uid;
            g_files[NFILES].gid = st.st_gid;

            /* Construct mode string */
            char m[16] = "----------";
            if (S_ISDIR(st.st_mode) || ent->d_type == DT_DIR) m[0] = 'd';
            if (st.st_mode & 0400) m[1] = 'r';
            if (st.st_mode & 0200) m[2] = 'w';
            if (st.st_mode & 0100) m[3] = 'x';
            if (st.st_mode & 0040) m[4] = 'r';
            if (st.st_mode & 0020) m[5] = 'w';
            if (st.st_mode & 0010) m[6] = 'x';
            if (st.st_mode & 0004) m[7] = 'r';
            if (st.st_mode & 0002) m[8] = 'w';
            if (st.st_mode & 0001) m[9] = 'x';

            /* Check POSIX ACL */
            acl_t acl = acl_get_file(full_path, 0);
            if (acl && acl->count > 3) {
                m[10] = '+';
                m[11] = '\0';
                g_files[NFILES].has_acl = 1;
            } else {
                m[10] = '\0';
                g_files[NFILES].has_acl = 0;
            }
            if (acl) acl_free(acl);
            strcpy(g_files[NFILES].mode, m);

            if (S_ISDIR(st.st_mode) || ent->d_type == DT_DIR) {
                strcpy(g_files[NFILES].type, "Folder");
                strcpy(g_files[NFILES].size, "-");
                g_files[NFILES].is_dir = 1;
            } else {
                if (file_has_ext(ent->d_name, "elf")) strcpy(g_files[NFILES].type, "Executable");
                else if (file_has_ext(ent->d_name, "txt") || file_has_ext(ent->d_name, "md")) strcpy(g_files[NFILES].type, "Text Doc");
                else if (file_has_ext(ent->d_name, "c") || file_has_ext(ent->d_name, "h")) strcpy(g_files[NFILES].type, "C Source");
                else if (file_has_ext(ent->d_name, "conf") || file_has_ext(ent->d_name, "cfg")) strcpy(g_files[NFILES].type, "Config");
                else if (file_has_ext(ent->d_name, "bmp") || file_has_ext(ent->d_name, "png")) strcpy(g_files[NFILES].type, "Image");
                else if (file_has_ext(ent->d_name, "wav")) strcpy(g_files[NFILES].type, "Audio");
                else strcpy(g_files[NFILES].type, "File");
                format_file_size((size_t)st.st_size, g_files[NFILES].size, sizeof(g_files[NFILES].size));
                g_files[NFILES].is_dir = 0;
            }
        } else {
            strcpy(g_files[NFILES].type, (ent->d_type == DT_DIR) ? "Folder" : "File");
            strcpy(g_files[NFILES].size, "-");
            strcpy(g_files[NFILES].mode, "-rw-r--r--");
            g_files[NFILES].is_dir = (ent->d_type == DT_DIR);
            g_files[NFILES].has_acl = 0;
        }
        NFILES++;
    }
    closedir(dir);

    /* Sort everything except the leading ".." entry */
    if (NFILES - dotdot > 1) {
        qsort(&g_files[dotdot], (size_t)(NFILES - dotdot), sizeof(file_entry_t), file_entry_cmp);
    }
}

static void activate_entry(int idx)
{
    if (idx < 0 || idx >= NFILES) return;

    if (g_files[idx].is_dir) {
        if (strcmp(g_files[idx].name, "..") == 0) {
            char *last_slash = strrchr(g_current_path, '/');
            if (last_slash && last_slash != g_current_path) {
                *last_slash = '\0';
                load_directory(g_current_path);
            } else {
                load_directory("/");
            }
        } else {
            char next_path[512];
            build_full_path(next_path, sizeof(next_path), g_files[idx].name);
            load_directory(next_path);
        }
    } else {
        const char *fn = g_files[idx].name;
        char full_path[512];
        build_full_path(full_path, sizeof(full_path), fn);

        if (file_has_ext(fn, "elf")) {
            uk_launch_app(&g_win, full_path);
        } else {
            uk_launch_app_arg(&g_win, resolve_app_for_file(fn), full_path);
        }
    }
}

static void show_file_properties(int idx)
{
    if (idx < 0 || idx >= NFILES) return;

    char full_path[512];
    build_full_path(full_path, sizeof(full_path), g_files[idx].name);

    acl_t acl = acl_get_file(full_path, 0);
    char *acl_txt = acl ? acl_to_text(acl, NULL) : NULL;

    snprintf(g_props_text, sizeof(g_props_text),
             "File: %s\nPath: %s\nType: %s | Size: %s\nMode: %s (UID %u, GID %u)\n\nPOSIX ACL Rules:\n%s",
             g_files[idx].name, full_path, g_files[idx].type, g_files[idx].size,
             g_files[idx].mode, (unsigned int)g_files[idx].uid, (unsigned int)g_files[idx].gid,
             acl_txt ? acl_txt : "(Standard base mode)");

    if (acl_txt) free(acl_txt);
    if (acl) acl_free(acl);

    g_show_props = true;
    start_modal_anim();
}

static void begin_rename(int idx)
{
    if (idx < 0 || idx >= NFILES) return;
    if (strcmp(g_files[idx].name, "..") == 0) return;
    strncpy(g_prompt_buf, g_files[idx].name, sizeof(g_prompt_buf) - 1);
    g_prompt_buf[sizeof(g_prompt_buf) - 1] = '\0';
    g_prompt_len = (int)strlen(g_prompt_buf);
    g_prompt_mode = PROMPT_RENAME;
}

static void begin_new_folder(void)
{
    g_prompt_buf[0] = '\0';
    g_prompt_len = 0;
    g_prompt_mode = PROMPT_NEW_FOLDER;
    start_modal_anim();
}

static void open_delete_confirm(void)
{
    g_confirm_delete = true;
    start_modal_anim();
}

static void commit_prompt(void)
{
    if (g_prompt_mode == PROMPT_RENAME && g_selected >= 0 && g_prompt_len > 0) {
        char old_path[512], new_path[512];
        build_full_path(old_path, sizeof(old_path), g_files[g_selected].name);
        build_full_path(new_path, sizeof(new_path), g_prompt_buf);
        if (sys_rename(old_path, new_path) < 0)
            uk_notify(&g_win, "Rename Failed", "Could not rename the item.");
        load_directory(g_current_path);
    } else if (g_prompt_mode == PROMPT_NEW_FOLDER && g_prompt_len > 0) {
        char new_path[512];
        build_full_path(new_path, sizeof(new_path), g_prompt_buf);
        if (sys_mkdir(new_path, 0755) < 0)
            uk_notify(&g_win, "New Folder Failed", "Could not create the folder.");
        load_directory(g_current_path);
    }
    g_prompt_mode = PROMPT_NONE;
}

static void create_new_note(void)
{
    char note_path[512];
    int n = 0;
    do {
        if (n == 0)
            snprintf(note_path, sizeof(note_path), "%s/note.txt", g_current_path);
        else
            snprintf(note_path, sizeof(note_path), "%s/note%d.txt", g_current_path, n);
        struct stat st;
        if (stat(note_path, &st) != 0) break; /* free name found */
        n++;
    } while (n < 1000);

    int fd = sys_open(note_path, 0x42 /* O_CREAT|O_WRONLY */, 0644);
    if (fd >= 0) {
        sys_write(fd, "AzamiOS Note\n", 13);
        sys_close(fd);
        load_directory(g_current_path);
    }
}

static void perform_delete(int idx)
{
    if (idx < 0 || idx >= NFILES) return;
    if (strcmp(g_files[idx].name, "..") == 0) return;

    char del_path[512];
    build_full_path(del_path, sizeof(del_path), g_files[idx].name);

    int rc = g_files[idx].is_dir ? sys_rmdir(del_path)  /* only removes empty dirs, by design */
                                  : sys_unlink(del_path);
    if (rc < 0)
        uk_notify(&g_win, "Delete Failed",
                  g_files[idx].is_dir ? "Folder is not empty." : "Could not delete the file.");

    load_directory(g_current_path);
}

/* Jump the selection to the next entry (wrapping) whose name starts with
 * `c`, case-insensitively — classic file-manager type-ahead. */
static void typeahead_jump(char c)
{
    if (NFILES == 0) return;
    int start = (g_selected < 0) ? 0 : g_selected + 1;
    for (int i = 0; i < NFILES; i++) {
        int idx = (start + i) % NFILES;
        if (tolower((unsigned char)g_files[idx].name[0]) == tolower((unsigned char)c)) {
            g_selected = idx;
            if (g_selected < g_scroll) g_scroll = g_selected;
            if (g_selected >= g_scroll + VISIBLE_ROWS) g_scroll = g_selected - VISIBLE_ROWS + 1;
            return;
        }
    }
}

static void move_selection(int delta)
{
    if (NFILES == 0) return;
    int idx = g_selected < 0 ? 0 : g_selected + delta;
    if (idx < 0) idx = 0;
    if (idx >= NFILES) idx = NFILES - 1;
    g_selected = idx;
    if (g_selected < g_scroll) g_scroll = g_selected;
    if (g_selected >= g_scroll + VISIBLE_ROWS) g_scroll = g_selected - VISIBLE_ROWS + 1;
}

static void draw_filemanager(void)
{
    unsigned int w = g_win.width;
    unsigned int h = g_win.height;

    if (g_scroll > NFILES - VISIBLE_ROWS) g_scroll = NFILES - VISIBLE_ROWS;
    if (g_scroll < 0) g_scroll = 0;

    uk_fill_rect(&g_win, 0, 0, (int)w, (int)h, UK_BASE);

    /* ── Title / Breadcrumb Bar ───────────────────────────────────────────── */
    uk_gradient_h(&g_win, 0, 0, (int)w, 36, UK_MANTLE, UK_CRUST);
    uk_hline(&g_win, 0, 36, (int)w, UK_SURFACE0);

    uk_draw_badge(&g_win, 12, 8, "Path", UK_SURFACE1, UK_MAUVE);

    /* Clickable breadcrumb segments: Root › home › azami › Desktop */
    g_crumb_count = 0;
    {
        int bx = 64;
        char accum[256] = "";
        char seg_path[256];

        if (g_crumb_count < MAX_CRUMBS) {
            const char *label = "Root";
            int tw = uk_strlen(label) * 8;
            g_crumbs[g_crumb_count].x0 = bx;
            g_crumbs[g_crumb_count].x1 = bx + tw;
            strcpy(g_crumbs[g_crumb_count].path, "/");
            uk_draw_text(&g_win, bx, 10, label, (g_crumb_count == 0 && strcmp(g_current_path, "/") == 0) ? UK_MAUVE : UK_TEXT);
            g_crumb_count++;
            bx += tw;
        }

        char path_copy[256];
        strncpy(path_copy, g_current_path, sizeof(path_copy) - 1);
        path_copy[sizeof(path_copy) - 1] = '\0';

        char *tok = strtok(path_copy, "/");
        while (tok && g_crumb_count < MAX_CRUMBS) {
            snprintf(seg_path, sizeof(seg_path), "%s/%s", accum, tok);
            strncpy(accum, seg_path, sizeof(accum) - 1);
            accum[sizeof(accum) - 1] = '\0';

            bx += 12;
            uk_draw_text(&g_win, bx, 10, ">", UK_OVERLAY0);
            bx += 12;

            int tw = uk_strlen(tok) * 8;
            bool is_last = (strcmp(seg_path, g_current_path) == 0);
            g_crumbs[g_crumb_count].x0 = bx;
            g_crumbs[g_crumb_count].x1 = bx + tw;
            strncpy(g_crumbs[g_crumb_count].path, seg_path, sizeof(g_crumbs[g_crumb_count].path) - 1);
            g_crumbs[g_crumb_count].path[sizeof(g_crumbs[g_crumb_count].path) - 1] = '\0';
            uk_draw_text(&g_win, bx, 10, tok, is_last ? UK_MAUVE : UK_TEXT);
            g_crumb_count++;
            bx += tw;

            tok = strtok(NULL, "/");
        }
    }

    /* ── Action Toolbar ───────────────────────────────────────────────────── */
    uk_fill_rect(&g_win, 0, 37, (int)w, 36, UK_MANTLE);
    uk_hline(&g_win, 0, 73, (int)w, UK_SURFACE0);

    int btn_x = TOOLBAR_X0;
    for (int i = 0; i < NUM_TOOLBAR; i++) {
        unsigned int col = UK_BTN_NORMAL;
        switch (g_toolbar[i].action) {
        case ACT_OPEN:
        case ACT_RENAME:
            col = (g_selected >= 0) ? UK_BTN_PRESSED : UK_BTN_NORMAL;
            break;
        case ACT_DELETE:
            col = (g_selected >= 0) ? UK_BTN_PRESSED : UK_BTN_NORMAL;
            break;
        default:
            break;
        }
        uk_draw_button(&g_win, btn_x, 42, g_toolbar[i].width, 26, g_toolbar[i].label, col);
        btn_x += g_toolbar[i].width + TOOLBAR_GAP;
    }

    /* ── Sidebar (Places) ─────────────────────────────────────────────────── */
    uk_fill_rect(&g_win, 0, 74, SIDEBAR_W, (int)h - 74, UK_MANTLE);
    uk_vline(&g_win, SIDEBAR_W, 74, (int)h - 74, UK_SURFACE0);

    uk_draw_text(&g_win, 12, 84, "PLACES", UK_OVERLAY0);

    for (int p = 0; p < g_num_places; p++) {
        int py = 104 + p * 26;
        bool is_sel = (p == g_selected_place);
        if (is_sel) {
            uk_fill_rounded_rect(&g_win, 8, py - 2, SIDEBAR_W - 16, 22, 6, UK_SURFACE0);
            uk_fill_rounded_rect(&g_win, 8, py + 2, 3, 14, 1, UK_MAUVE);
        }
        uk_draw_text(&g_win, 20, py + 2, g_places[p].icon, is_sel ? UK_MAUVE : UK_SUBTEXT0);
        uk_draw_text(&g_win, 36, py + 2, g_places[p].label, is_sel ? UK_TEXT : UK_SUBTEXT1);
    }

    /* ── Main List Column Headers ─────────────────────────────────────────── */
    int col_y = LIST_OY + 4;
    uk_draw_text(&g_win, LIST_OX + 24, col_y, "Name", UK_OVERLAY0);
    uk_draw_text(&g_win, LIST_OX + 240, col_y, "Type", UK_OVERLAY0);
    uk_draw_text(&g_win, LIST_OX + 340, col_y, "Size", UK_OVERLAY0);
    uk_draw_text(&g_win, LIST_OX + 430, col_y, "Permissions", UK_OVERLAY0);

    uk_hline(&g_win, LIST_OX, col_y + 18, (int)w - LIST_OX - 10, UK_SURFACE0);

    /* ── File Entries ─────────────────────────────────────────────────────── */
    int list_y = col_y + 24;
    for (int i = 0; i < VISIBLE_ROWS && (i + g_scroll) < NFILES; i++) {
        int idx = i + g_scroll;
        int ry = list_y + i * ROW_H;
        bool is_sel = (idx == g_selected);
        bool is_hov = (idx == g_hovered);

        if (is_sel) {
            uk_fill_rounded_rect(&g_win, LIST_OX, ry - 1, (int)w - LIST_OX - 10, ROW_H - 2, 4, UK_SURFACE0);
            uk_fill_rounded_rect(&g_win, LIST_OX, ry + 2, 3, ROW_H - 8, 1, UK_MAUVE);
        } else if (is_hov) {
            uk_fill_rounded_rect(&g_win, LIST_OX, ry - 1, (int)w - LIST_OX - 10, ROW_H - 2, 4, 0xFF222230);
        }

        /* Icon */
        if (g_files[idx].is_dir) {
            uk_draw_text(&g_win, LIST_OX + 8, ry + 2, "[D]", UK_YELLOW);
        } else if (file_has_ext(g_files[idx].name, "elf")) {
            uk_draw_text(&g_win, LIST_OX + 8, ry + 2, "[*]", UK_GREEN);
        } else {
            uk_draw_text(&g_win, LIST_OX + 8, ry + 2, "[F]", UK_BLUE);
        }

        /* Name — while renaming this row, show the live edit textbox instead */
        if (is_sel && g_prompt_mode == PROMPT_RENAME) {
            uk_draw_textbox(&g_win, LIST_OX + 32, ry - 2, 200, ROW_H, g_prompt_buf, "", 1, g_prompt_len);
        } else {
            uk_draw_text(&g_win, LIST_OX + 34, ry + 2, g_files[idx].name, is_sel ? UK_TEXT : UK_SUBTEXT1);
        }

        /* Type */
        uk_draw_text(&g_win, LIST_OX + 240, ry + 2, g_files[idx].type, UK_OVERLAY1);

        /* Size */
        uk_draw_text(&g_win, LIST_OX + 340, ry + 2, g_files[idx].size, UK_OVERLAY0);

        /* Permissions & ACL badge */
        unsigned int perm_col = g_files[idx].has_acl ? UK_MAUVE : UK_OVERLAY1;
        uk_draw_text(&g_win, LIST_OX + 430, ry + 2, g_files[idx].mode, perm_col);
    }

    /* ── Scrollbar ─────────────────────────────────────────────────────────── */
    if (NFILES > VISIBLE_ROWS) {
        int track_h = VISIBLE_ROWS * ROW_H;
        int thumb_h = (track_h * VISIBLE_ROWS) / NFILES;
        int thumb_pos = (g_scroll * track_h) / NFILES;
        uk_draw_scrollbar(&g_win, SCROLLBAR_X, list_y, track_h, thumb_pos, thumb_h);
    }

    /* ── Bottom Status Bar ────────────────────────────────────────────────── */
    uk_fill_rect(&g_win, SIDEBAR_W, (int)h - 24, (int)w - SIDEBAR_W, 24, UK_MANTLE);
    uk_hline(&g_win, SIDEBAR_W, (int)h - 24, (int)w - SIDEBAR_W, UK_SURFACE0);

    struct statvfs svfs;
    char free_str[48] = "";
    if (statvfs(g_current_path, &svfs) == 0) {
        unsigned long long bsize = svfs.f_frsize ? svfs.f_frsize : (svfs.f_bsize ? svfs.f_bsize : 1024ULL);
        unsigned long long free_mb = ((unsigned long long)svfs.f_bavail * bsize) / (1024ULL * 1024ULL);
        snprintf(free_str, sizeof(free_str), "Free: %llu MB", free_mb);
    }

    char status_str[200];
    if (g_selected >= 0 && g_selected < NFILES) {
        snprintf(status_str, sizeof(status_str), "%s  —  %s, %s, %s",
                 g_files[g_selected].name, g_files[g_selected].type,
                 g_files[g_selected].size, g_files[g_selected].mode);
    } else {
        snprintf(status_str, sizeof(status_str), "%d item(s) in %s", NFILES, g_current_path);
    }
    uk_draw_text(&g_win, SIDEBAR_W + 12, (int)h - 18, status_str, UK_SUBTEXT0);

    if (free_str[0]) {
        int flen = uk_strlen(free_str);
        uk_draw_text(&g_win, (int)w - flen * 8 - 16, (int)h - 18, free_str, UK_OVERLAY1);
    }

    /* ── New Folder Prompt Modal ──────────────────────────────────────────── */
    if (g_prompt_mode == PROMPT_NEW_FOLDER) {
        int modal_w = 320, modal_h = 110;
        int mx = ((int)w - modal_w) / 2;
        int my = ((int)h - modal_h) / 2 + modal_anim_dy();
        uk_fill_rounded_rect(&g_win, mx, my, modal_w, modal_h, 10, UK_CRUST);
        uk_draw_rounded_rect_outline(&g_win, mx, my, modal_w, modal_h, 10, UK_MAUVE);
        uk_draw_text(&g_win, mx + 16, my + 14, "New Folder Name", UK_MAUVE);
        uk_draw_textbox(&g_win, mx + 16, my + 38, modal_w - 32, 28, g_prompt_buf, "folder name", 1, g_prompt_len);
        uk_draw_text(&g_win, mx + 16, my + 80, "Enter to create, Esc to cancel", UK_OVERLAY0);
    }

    /* ── Delete Confirmation Modal ────────────────────────────────────────── */
    if (g_confirm_delete && g_selected >= 0) {
        int modal_w = 340, modal_h = 120;
        int mx = ((int)w - modal_w) / 2;
        int my = ((int)h - modal_h) / 2 + modal_anim_dy();
        char msg[128];
        snprintf(msg, sizeof(msg), "\"%s\" will be permanently removed.", g_files[g_selected].name);
        uk_draw_confirm_dialog(&g_win, mx, my, modal_w, modal_h,
                               "Delete this item?", UK_RED, msg, "Delete", NULL, NULL);
    }

    /* ── Properties Modal Dialog (if open) ────────────────────────────────── */
    if (g_show_props) {
        int modal_w = 460;
        int modal_h = 240;
        int mx = ((int)w - modal_w) / 2;
        int my = ((int)h - modal_h) / 2 + modal_anim_dy();

        uk_fill_rounded_rect(&g_win, mx, my, modal_w, modal_h, 10, UK_CRUST);
        uk_draw_rounded_rect_outline(&g_win, mx, my, modal_w, modal_h, 10, UK_MAUVE);

        uk_fill_rounded_rect(&g_win, mx, my, modal_w, 32, 10, UK_MANTLE);
        uk_draw_text(&g_win, mx + 16, my + 8, "File Properties & POSIX ACLs", UK_MAUVE);
        uk_draw_button(&g_win, mx + modal_w - 40, my + 4, 30, 24, "X", UK_BTN_PRESSED);

        /* Render multi-line property text */
        char text_copy[512];
        strncpy(text_copy, g_props_text, sizeof(text_copy) - 1);
        text_copy[sizeof(text_copy) - 1] = '\0';

        char *line = strtok(text_copy, "\n");
        int ly = my + 44;
        while (line && ly < my + modal_h - 20) {
            uk_draw_text(&g_win, mx + 16, ly, line, UK_TEXT);
            ly += 18;
            line = strtok(NULL, "\n");
        }
    }

    /* ── Right-click Context Menu ─────────────────────────────────────────── */
    if (g_ctx_open) {
        if (g_ctx_target >= 0)
            uk_draw_context_menu(&g_win, g_ctx_x, g_ctx_y, g_ctx_items_file, NUM_CTX_FILE, g_ctx_hover);
        else
            uk_draw_context_menu(&g_win, g_ctx_x, g_ctx_y, g_ctx_items_blank, NUM_CTX_BLANK, g_ctx_hover);
    }

    uk_invalidate(&g_win);
}

/* Returns true if (mx,my) landed on a toolbar button, and if so runs it. */
static bool handle_toolbar_click(int mx, int my)
{
    if (my < 42 || my > 68) return false;

    int btn_x = TOOLBAR_X0;
    for (int i = 0; i < NUM_TOOLBAR; i++) {
        int x0 = btn_x;
        int x1 = btn_x + g_toolbar[i].width;
        if (mx >= x0 && mx <= x1) {
            switch (g_toolbar[i].action) {
            case ACT_OPEN:
                if (g_selected >= 0) activate_entry(g_selected);
                break;
            case ACT_EDIT:
                if (g_selected >= 0 && !g_files[g_selected].is_dir) {
                    char edit_path[512];
                    build_full_path(edit_path, sizeof(edit_path), g_files[g_selected].name);
                    uk_launch_app_arg(&g_win, "/bin/texteditor.elf", edit_path);
                } else {
                    uk_launch_app(&g_win, "/bin/texteditor.elf");
                }
                break;
            case ACT_TERMINAL:
                uk_launch_app(&g_win, "/bin/terminal.elf");
                break;
            case ACT_NEW_FOLDER:
                begin_new_folder();
                break;
            case ACT_NEW_NOTE:
                create_new_note();
                break;
            case ACT_RENAME:
                if (g_selected >= 0) begin_rename(g_selected);
                break;
            case ACT_DELETE:
                if (g_selected >= 0) open_delete_confirm();
                break;
            }
            return true;
        }
        btn_x += g_toolbar[i].width + TOOLBAR_GAP;
    }
    return false;
}

/* Returns true if (mx,my) landed on a breadcrumb segment, and if so navigates. */
static bool handle_breadcrumb_click(int mx, int my)
{
    if (my < 6 || my > 26) return false;
    for (int i = 0; i < g_crumb_count; i++) {
        if (mx >= g_crumbs[i].x0 && mx <= g_crumbs[i].x1) {
            load_directory(g_crumbs[i].path);
            return true;
        }
    }
    return false;
}

/* Mirrors uk_draw_context_menu()'s own width computation so hit-testing
 * never drifts from what was actually drawn. */
static int ctx_menu_width(const char **items, int count)
{
    int max_w = 120;
    for (int i = 0; i < count; i++) {
        int l = uk_strlen(items[i]) * 8 + 24;
        if (l > max_w) max_w = l;
    }
    return max_w;
}

static void open_context_menu(int mx, int my, int target_idx)
{
    bool is_file_menu = (target_idx >= 0);
    int count = is_file_menu ? NUM_CTX_FILE : NUM_CTX_BLANK;
    int menu_w = is_file_menu ? ctx_menu_width(g_ctx_items_file, NUM_CTX_FILE)
                               : ctx_menu_width(g_ctx_items_blank, NUM_CTX_BLANK);
    int menu_h = count * 28 + 8;

    if (mx + menu_w > (int)g_win.width) mx = (int)g_win.width - menu_w;
    if (my + menu_h > (int)g_win.height) my = (int)g_win.height - menu_h;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;

    g_ctx_open = true;
    g_ctx_x = mx;
    g_ctx_y = my;
    g_ctx_hover = -1;
    g_ctx_target = target_idx;
    if (target_idx >= 0) g_selected = target_idx;
}

static void handle_context_menu_select(int item)
{
    g_ctx_open = false;
    if (g_ctx_target >= 0) {
        switch (item) {
        case 0: activate_entry(g_ctx_target); break;                 /* Open */
        case 1: {                                                    /* Edit */
            if (!g_files[g_ctx_target].is_dir) {
                char edit_path[512];
                build_full_path(edit_path, sizeof(edit_path), g_files[g_ctx_target].name);
                uk_launch_app_arg(&g_win, "/bin/texteditor.elf", edit_path);
            } else {
                uk_launch_app(&g_win, "/bin/texteditor.elf");
            }
            break;
        }
        case 2: {                                                    /* Copy Path */
            char copy_path[512];
            build_full_path(copy_path, sizeof(copy_path), g_files[g_ctx_target].name);
            uk_clipboard_set(&g_win, copy_path);
            break;
        }
        case 3: {                                                    /* Open Terminal */
            if (g_files[g_ctx_target].is_dir) {
                char term_path[512];
                build_full_path(term_path, sizeof(term_path), g_files[g_ctx_target].name);
                uk_launch_app_arg(&g_win, "/bin/terminal.elf", term_path);
            } else {
                uk_launch_app_arg(&g_win, "/bin/terminal.elf", g_current_path);
            }
            break;
        }
        case 4: begin_rename(g_ctx_target); break;                   /* Rename */
        case 5: show_file_properties(g_ctx_target); break;           /* Properties */
        case 6: g_selected = g_ctx_target; open_delete_confirm(); break;   /* Delete */
        }
    } else {
        switch (item) {
        case 0: begin_new_folder(); break;      /* New Folder */
        case 1: create_new_note(); break;       /* New Note */
        case 2: uk_launch_app_arg(&g_win, "/bin/terminal.elf", g_current_path); break; /* Open Terminal */
        case 3: load_directory(g_current_path); break; /* Refresh */
        }
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    az_fb_info_t fb;
    unsigned int sw = 1280, sh = 800;
    if (az_fb_info(&fb) == 0 && fb.width > 0 && fb.height > 0) {
        sw = fb.width;
        sh = fb.height;
    }

    int win_x = (int)(sw - WIN_W) / 2;
    int win_y = (int)(sh - WIN_H) / 2;

    if (uk_window_connect(&g_win, "Files", win_x, win_y, WIN_W, WIN_H, MAP_ADDR, SERVER_CHAN) < 0) {
        return 1;
    }

    /* Drives the modal entrance animation — see modal_anim_dy(). */
    az_set_timer(g_win.client_chan, 100, 0);

    load_places_config();
    load_file_assoc_config();
    load_directory("/");
    draw_filemanager();

    az_ipc_msg_t raw_msg;
    az_wm_msg_t *msg = (az_wm_msg_t *)&raw_msg;
    unsigned int prev_buttons = 0;

    for (;;) {
        /* See sysmon's identical fix: `continue` here means a closed window
         * (channel -> -EPIPE on every further call, never blocking again)
         * never reaches sys_exit and instead retries as fast as the CPU
         * allows, forever. `break` falls through to the same `return 0;`
         * every sibling app in the DE already uses for this. */
        if (az_channel_recv(g_win.client_chan, &raw_msg) != 0) break;

        switch (msg->type) {
        case AZ_WM_MOUSE_EVENT: {
            int mx = msg->mouse.abs_x;
            int my = msg->mouse.abs_y;
            unsigned int buttons = msg->mouse.buttons;
            bool lclick = (buttons & AZ_MOUSE_BTN_LEFT) && !(prev_buttons & AZ_MOUSE_BTN_LEFT);
            bool rclick = (buttons & AZ_MOUSE_BTN_RIGHT) && !(prev_buttons & AZ_MOUSE_BTN_RIGHT);

            /* Wheel scroll works anywhere over the file list, regardless of
             * button state, and closes any open popups it scrolls under. */
            if (msg->mouse.wheel != 0 && !g_show_props && !g_ctx_open &&
                g_prompt_mode == PROMPT_NONE && !g_confirm_delete &&
                mx >= LIST_OX && my >= LIST_OY) {
                g_scroll += (msg->mouse.wheel > 0) ? 2 : -2;
                if (g_scroll < 0) g_scroll = 0;
                if (g_scroll > NFILES - VISIBLE_ROWS) g_scroll = NFILES - VISIBLE_ROWS;
                if (g_scroll < 0) g_scroll = 0;
                draw_filemanager();
            }

            if (rclick) {
                if (g_show_props) { g_show_props = false; }
                if (g_prompt_mode != PROMPT_NONE) { g_prompt_mode = PROMPT_NONE; }
                g_confirm_delete = false;

                if (mx >= LIST_OX && my >= (LIST_OY + 28)) {
                    int row = (my - (LIST_OY + 28)) / ROW_H;
                    int idx = row + g_scroll;
                    open_context_menu(mx, my, (idx >= 0 && idx < NFILES) ? idx : -1);
                } else if (mx >= LIST_OX) {
                    open_context_menu(mx, my, -1);
                }
                draw_filemanager();
                break;
            }

            if (lclick) {
                if (g_ctx_open) {
                    /* Resolve click against whichever menu is showing */
                    bool is_file_menu = (g_ctx_target >= 0);
                    int count = is_file_menu ? NUM_CTX_FILE : NUM_CTX_BLANK;
                    int menu_w = is_file_menu ? ctx_menu_width(g_ctx_items_file, NUM_CTX_FILE)
                                               : ctx_menu_width(g_ctx_items_blank, NUM_CTX_BLANK);
                    int item_h = 28;
                    if (my >= g_ctx_y + 4 && my < g_ctx_y + 4 + count * item_h &&
                        mx >= g_ctx_x && mx < g_ctx_x + menu_w) {
                        int item = (my - (g_ctx_y + 4)) / item_h;
                        handle_context_menu_select(item);
                    } else {
                        g_ctx_open = false;
                    }
                    draw_filemanager();
                    break;
                }

                if (g_confirm_delete) {
                    int modal_w = 340, modal_h = 120;
                    int mmx = ((int)g_win.width - modal_w) / 2;
                    int mmy = ((int)g_win.height - modal_h) / 2 + modal_anim_dy();
                    uk_rect_t cancel_rect, delete_rect;
                    uk_confirm_dialog_layout(mmx, mmy, modal_w, modal_h, "Delete",
                                             &cancel_rect, &delete_rect);
                    if (uk_hit_rect(cancel_rect, mx, my)) {
                        g_confirm_delete = false;
                        draw_filemanager();
                    } else if (uk_hit_rect(delete_rect, mx, my)) {
                        g_confirm_delete = false;
                        perform_delete(g_selected);
                        draw_filemanager();
                    }
                    break;
                }

                if (g_prompt_mode != PROMPT_NONE) {
                    /* Clicking outside the modal cancels it */
                    g_prompt_mode = PROMPT_NONE;
                    draw_filemanager();
                    break;
                }

                if (g_show_props) {
                    /* Check close button on properties modal */
                    int modal_w = 460;
                    int modal_h = 240;
                    int px = ((int)g_win.width - modal_w) / 2;
                    int py = ((int)g_win.height - modal_h) / 2 + modal_anim_dy();
                    if (mx >= px + modal_w - 40 && mx <= px + modal_w - 10 && my >= py + 4 && my <= py + 28) {
                        g_show_props = false;
                        draw_filemanager();
                    }
                    break;
                }

                if (handle_breadcrumb_click(mx, my)) {
                    draw_filemanager();
                    break;
                }

                if (handle_toolbar_click(mx, my)) {
                    draw_filemanager();
                    break;
                }

                /* Sidebar Places click */
                if (mx < SIDEBAR_W && my >= 104) {
                    int p = (my - 104) / 26;
                    if (p >= 0 && p < g_num_places) {
                        g_selected_place = p;
                        load_directory(g_places[p].path);
                        draw_filemanager();
                    }
                    break;
                }

                /* File list item click */
                if (mx >= LIST_OX && my >= (LIST_OY + 28)) {
                    int row = (my - (LIST_OY + 28)) / ROW_H;
                    int idx = row + g_scroll;
                    if (idx >= 0 && idx < NFILES) {
                        if (g_selected == idx) {
                            /* Double-click / re-click to open */
                            activate_entry(idx);
                        } else {
                            g_selected = idx;
                            draw_filemanager();
                        }
                    }
                }
            } else if (buttons == 0) {
                if (mx >= LIST_OX && my >= (LIST_OY + 28)) {
                    int row = (my - (LIST_OY + 28)) / ROW_H;
                    int idx = row + g_scroll;
                    if (idx >= 0 && idx < NFILES && idx != g_hovered) {
                        g_hovered = idx;
                        draw_filemanager();
                    }
                }
                if (g_ctx_open) {
                    bool is_file_menu = (g_ctx_target >= 0);
                    int count = is_file_menu ? NUM_CTX_FILE : NUM_CTX_BLANK;
                    int menu_w = is_file_menu ? ctx_menu_width(g_ctx_items_file, NUM_CTX_FILE)
                                               : ctx_menu_width(g_ctx_items_blank, NUM_CTX_BLANK);
                    int item_h = 28;
                    int hov = -1;
                    if (my >= g_ctx_y + 4 && my < g_ctx_y + 4 + count * item_h &&
                        mx >= g_ctx_x && mx < g_ctx_x + menu_w) {
                        hov = (my - (g_ctx_y + 4)) / item_h;
                    }
                    if (hov != g_ctx_hover) {
                        g_ctx_hover = hov;
                        draw_filemanager();
                    }
                }
            }
            prev_buttons = buttons;
            break;
        }

        case AZ_WM_KEY_EVENT: {
            if (!msg->key.pressed) break;
            unsigned char kc = msg->key.keycode;

            /* Any keyboard interaction other than dismissing it moves focus
             * away from an open context menu. */
            if (g_ctx_open && kc != KEY_ESC) g_ctx_open = false;

            /* ── Modal prompt (rename / new folder) captures all typing ──── */
            if (g_prompt_mode != PROMPT_NONE) {
                if (kc == KEY_ESC) {
                    g_prompt_mode = PROMPT_NONE;
                } else if (kc == '\n' || kc == '\r') {
                    commit_prompt();
                } else if (kc == '\b' || kc == 127) {
                    if (g_prompt_len > 0) g_prompt_buf[--g_prompt_len] = '\0';
                } else if (kc >= 32 && kc <= 126 && g_prompt_len < (int)sizeof(g_prompt_buf) - 1) {
                    g_prompt_buf[g_prompt_len++] = (char)kc;
                    g_prompt_buf[g_prompt_len] = '\0';
                }
                draw_filemanager();
                break;
            }

            if (g_confirm_delete) {
                if (kc == KEY_ESC) {
                    g_confirm_delete = false;
                } else if (kc == '\n' || kc == '\r') {
                    g_confirm_delete = false;
                    perform_delete(g_selected);
                }
                draw_filemanager();
                break;
            }

            if (kc == KEY_ESC) {
                if (g_ctx_open) {
                    g_ctx_open = false;
                } else if (g_show_props) {
                    g_show_props = false;
                } else {
                    sys_exit(0);
                }
            } else if (kc == '\n' || kc == '\r') {
                if (g_selected >= 0) activate_entry(g_selected);
            } else if (kc == '\b') { /* Backspace */
                char *last_slash = strrchr(g_current_path, '/');
                if (last_slash && last_slash != g_current_path) {
                    *last_slash = '\0';
                    load_directory(g_current_path);
                } else {
                    load_directory("/");
                }
            } else if (kc == KEY_UP) {
                move_selection(-1);
            } else if (kc == KEY_DOWN) {
                move_selection(1);
            } else if (kc == KEY_HOME) {
                g_selected = (NFILES > 0) ? 0 : -1;
                g_scroll = 0;
            } else if (kc == KEY_END) {
                g_selected = NFILES - 1;
                g_scroll = (NFILES > VISIBLE_ROWS) ? NFILES - VISIBLE_ROWS : 0;
            } else if (kc == KEY_DELETE) {
                if (g_selected >= 0) open_delete_confirm();
            } else if (kc == KEY_F2) {
                if (g_selected >= 0) begin_rename(g_selected);
            } else if (kc == 'i' || kc == 'p') {
                if (g_selected >= 0) show_file_properties(g_selected);
            } else if (kc >= 32 && kc <= 126) {
                typeahead_jump((char)kc);
            }
            draw_filemanager();
            break;
        }

        case AZ_WM_TIMER_TICK: {
            if (g_modal_anim_step < MODAL_ANIM_STEPS) {
                g_modal_anim_step++;
                draw_filemanager();
            }
            break;
        }

        case AZ_WM_WINDOW_RESIZED: {
            /* A failed remap leaves no valid surface to draw into — skip
             * the redraw rather than paint into it, matching how this loop
             * has no AZ_WM_DESTROY_WINDOW case either (nothing here tears
             * the window down itself; that happens elsewhere). */
            if (uk_handle_resize(&g_win, msg)) {
                draw_filemanager();
            }
            break;
        }

        default:
            break;
        }
    }

    return 0;
}
