/* ============================================================================
 * AzamiOS — Desktop Theme Subsystem Implementation
 * File: userland/libc/theme.c
 *
 * Directory scan + key=value parsing mirrors font.c's az_font_scan_dirs():
 * *.theme files are discovered under AZ_THEME_DIR_SYSTEM then
 * AZ_THEME_DIR_PERSISTENT, parsed with the same "# comment / key=value,
 * whitespace-trimmed" grammar every /etc *.conf file in this OS already uses (see
 * config.elf's get_key_from_file()), and cached once per process so the
 * hot compositor/UI paths never touch disk after the first call.
 * ============================================================================ */

#if __has_include(<azami/theme.h>)
#include <azami/theme.h>
#elif __has_include("include/azami/theme.h")
#include "include/azami/theme.h"
#elif __has_include("userland/libc/include/azami/theme.h")
#include "userland/libc/include/azami/theme.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>

/* ── Built-in fallback palette ─────────────────────────────────────────────
 * Used only when disk has no *.theme files at all (fresh/incomplete rootfs,
 * or /usr and /hdd both unmounted) so the desktop never boots with an
 * undefined palette. Values match the five themes this OS has always
 * shipped, so an existing theme_id=0..4 in /etc/desktop.conf keeps meaning
 * exactly what it used to. */
static const az_theme_t s_builtin_themes[5] = {
    {
        .name = "Catppuccin Mocha",
        .crust = 0xFF11111B, .mantle = 0xFF181825, .base = 0xFF1E1E2E,
        .surface0 = 0xFF313244, .surface1 = 0xFF45475A, .surface2 = 0xFF585B70,
        .overlay0 = 0xFF6C7086, .overlay1 = 0xFF7F849C, .text = 0xFFCDD6F4,
        .accent = 0xFFCBA6F7, .accent_sec = 0xFFFAB387,
        .red = 0xFFF38BA8, .green = 0xFFA6E3A1, .yellow = 0xFFF9E2AF, .blue = 0xFF89B4FA,
    },
    {
        .name = "Catppuccin Latte",
        .crust = 0xFFDCE0E8, .mantle = 0xFFE6E9EF, .base = 0xFFEFF1F5,
        .surface0 = 0xFFCCD0DA, .surface1 = 0xFFBCC0CC, .surface2 = 0xFFACB0BE,
        .overlay0 = 0xFF9CA0B0, .overlay1 = 0xFF8C8FA1, .text = 0xFF4C4F69,
        .accent = 0xFF8839EF, .accent_sec = 0xFFFE640B,
        .red = 0xFFD20F39, .green = 0xFF40A02B, .yellow = 0xFFDF8E1D, .blue = 0xFF1E66F5,
    },
    {
        .name = "Nord Arctic",
        .crust = 0xFF242933, .mantle = 0xFF2E3440, .base = 0xFF3B4252,
        .surface0 = 0xFF434C5E, .surface1 = 0xFF4C566A, .surface2 = 0xFF5A657D,
        .overlay0 = 0xFF7885A0, .overlay1 = 0xFF9AA7C0, .text = 0xFFECEFF4,
        .accent = 0xFF88C0D0, .accent_sec = 0xFF81A1C1,
        .red = 0xFFBF616A, .green = 0xFFA3BE8C, .yellow = 0xFFEBCB8B, .blue = 0xFF5E81AC,
    },
    {
        .name = "Cyberpunk Neon",
        .crust = 0xFF05050A, .mantle = 0xFF0D0D18, .base = 0xFF141424,
        .surface0 = 0xFF202038, .surface1 = 0xFF2E2E50, .surface2 = 0xFF424270,
        .overlay0 = 0xFF6868A0, .overlay1 = 0xFF8F8FD0, .text = 0xFFF0F6FC,
        .accent = 0xFF00FFCC, .accent_sec = 0xFFFF007F,
        .red = 0xFFFF2A6D, .green = 0xFF05FFA1, .yellow = 0xFFFFE600, .blue = 0xFF00F0FF,
    },
    {
        .name = "OLED Pure Dark",
        .crust = 0xFF000000, .mantle = 0xFF050505, .base = 0xFF0A0A0A,
        .surface0 = 0xFF181818, .surface1 = 0xFF242424, .surface2 = 0xFF323232,
        .overlay0 = 0xFF555555, .overlay1 = 0xFF777777, .text = 0xFFFFFFFF,
        .accent = 0xFF3B82F6, .accent_sec = 0xFF10B981,
        .red = 0xFFEF4444, .green = 0xFF22C55E, .yellow = 0xFFEAB308, .blue = 0xFF60A5FA,
    },
};
#define NUM_BUILTIN_THEMES ((int)(sizeof(s_builtin_themes) / sizeof(s_builtin_themes[0])))

/* ── *.theme parsing ───────────────────────────────────────────────────────
 * Applies one "key=value" line to an az_theme_t already seeded with the
 * Mocha defaults, so a file that only overrides `accent=` still produces a
 * fully-populated, sane palette instead of a struct full of zeroed (opaque
 * black, alpha=0) colors. */
static void apply_theme_kv(az_theme_t *t, const char *key, const char *val)
{
    if (strcmp(key, "name") == 0) {
        strncpy(t->name, val, sizeof(t->name) - 1);
        t->name[sizeof(t->name) - 1] = '\0';
        return;
    }

    /* Every other key is a 0xAARRGGBB color; strtoul with base 0 accepts
     * the "0x" prefix these files always use and silently reads plain
     * decimal too, matching how config.elf treats every other .conf value. */
    uint32_t v = (uint32_t)strtoul(val, NULL, 0);
    if      (strcmp(key, "crust")      == 0) t->crust      = v;
    else if (strcmp(key, "mantle")     == 0) t->mantle     = v;
    else if (strcmp(key, "base")       == 0) t->base       = v;
    else if (strcmp(key, "surface0")   == 0) t->surface0   = v;
    else if (strcmp(key, "surface1")   == 0) t->surface1   = v;
    else if (strcmp(key, "surface2")   == 0) t->surface2   = v;
    else if (strcmp(key, "overlay0")   == 0) t->overlay0   = v;
    else if (strcmp(key, "overlay1")   == 0) t->overlay1   = v;
    else if (strcmp(key, "text")       == 0) t->text       = v;
    else if (strcmp(key, "accent")     == 0) t->accent     = v;
    else if (strcmp(key, "accent_sec") == 0) t->accent_sec = v;
    else if (strcmp(key, "red")        == 0) t->red        = v;
    else if (strcmp(key, "green")      == 0) t->green      = v;
    else if (strcmp(key, "yellow")     == 0) t->yellow     = v;
    else if (strcmp(key, "blue")       == 0) t->blue       = v;
    /* Unknown keys are ignored rather than rejected: a theme file from a
     * future AzamiOS version with extra roles still loads on an older one. */
}

/* Parses one *.theme file into *out. Returns 0 on success (file opened and
 * had at least a name), -1 if it couldn't be read at all. A file with a
 * name but no color keys still succeeds, inheriting the Mocha defaults
 * apply_theme_kv() seeded — never a half-black palette. */
static int parse_theme_file(const char *path, az_theme_t *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    *out = s_builtin_themes[0];
    out->name[0] = '\0';

    char line[192];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '[' || *p == '\0' || *p == '\n' || *p == '\r') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;

        char *kend = key + strlen(key);
        while (kend > key && (kend[-1] == ' ' || kend[-1] == '\t')) *--kend = '\0';

        size_t vlen = strlen(val);
        while (vlen > 0 && (val[vlen - 1] == '\n' || val[vlen - 1] == '\r' ||
                            val[vlen - 1] == ' '  || val[vlen - 1] == '\t')) val[--vlen] = '\0';
        while (*val == ' ' || *val == '\t') val++;

        apply_theme_kv(out, key, val);
    }
    fclose(f);

    if (out->name[0] == '\0') {
        /* No `name=` line: derive a display name from the filename so the
         * theme still shows up sensibly in a picker. */
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        strncpy(out->name, base, sizeof(out->name) - 1);
        out->name[sizeof(out->name) - 1] = '\0';
        char *dot = strrchr(out->name, '.');
        if (dot) *dot = '\0';
    }
    return 0;
}

static void scan_theme_dir(const char *dirpath, az_theme_t *out_list, int *count, int max_themes)
{
    if (*count >= max_themes) return;

    DIR *dir = opendir(dirpath);
    if (!dir) return;

    struct dirent *ent;
    while (*count < max_themes && (ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        const char *ext = strrchr(ent->d_name, '.');
        if (!ext || strcmp(ext, AZ_THEME_EXT) != 0) continue;

        char fullpath[288];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, ent->d_name);

        if (parse_theme_file(fullpath, &out_list[*count]) == 0) {
            (*count)++;
        }
    }
    closedir(dir);
}

int az_theme_scan_dirs(az_theme_t *out_list, int max_themes)
{
    if (!out_list || max_themes <= 0) return 0;
    int count = 0;

    scan_theme_dir(AZ_THEME_DIR_SYSTEM, out_list, &count, max_themes);
    scan_theme_dir(AZ_THEME_DIR_PERSISTENT, out_list, &count, max_themes);

    return count;
}

/* ── Process-local cache ───────────────────────────────────────────────────
 * Mirrors the `s_font_count < 0` sentinel pattern apps already use around
 * az_font_scan_dirs() (see settings/main.c), but centralized here so every
 * caller in the process shares one scan instead of each TU re-walking disk. */
static az_theme_t s_cache[AZ_THEME_MAX];
static int        s_cache_count = -1;

static void ensure_cache(void)
{
    if (s_cache_count >= 0) return;

    s_cache_count = az_theme_scan_dirs(s_cache, AZ_THEME_MAX);
    if (s_cache_count <= 0) {
        /* Nothing on disk (or no filesystem mounted yet) — fall back to the
         * compiled-in defaults so callers always get a usable palette. */
        for (int i = 0; i < NUM_BUILTIN_THEMES; i++) s_cache[i] = s_builtin_themes[i];
        s_cache_count = NUM_BUILTIN_THEMES;
    }
}

void az_theme_reload(void)
{
    s_cache_count = -1;
}

int az_theme_count(void)
{
    ensure_cache();
    return s_cache_count;
}

const az_theme_t *az_theme_get(unsigned int theme_id)
{
    ensure_cache();
    if (theme_id >= (unsigned int)s_cache_count) theme_id = 0;
    return &s_cache[theme_id];
}
