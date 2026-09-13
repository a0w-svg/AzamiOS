/* ============================================================================
 * AzamiOS — Desktop Theme Subsystem
 * File: userland/libc/include/azami/theme.h
 *
 * Themes used to be a `static const` C array compiled into ui_kit.h and
 * duplicated (with a second, narrower struct) inside the Settings app. That
 * meant adding or tweaking a theme required a recompile of every app that
 * included ui_kit.h, and the two copies could (and did) drift apart.
 *
 * Themes now live on disk as plain key=value text files — the same
 * "POSIX INI" convention every other /etc *.conf file in this OS already uses —
 * and are scanned once per process, on demand, exactly like the font
 * subsystem (see azami/font.h + az_font_scan_dirs()). A theme is just data:
 * dropping a new *.theme file into /usr/share/themes or /hdd/themes makes it
 * available with no rebuild.
 * ============================================================================ */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Standard File Locations on Drive ─────────────────────────────────────── */
#define AZ_THEME_DIR_SYSTEM     "/usr/share/themes"
#define AZ_THEME_DIR_PERSISTENT "/hdd/themes"
#define AZ_THEME_EXT            ".theme"

/* Hard ceiling on how many themes az_theme_count()/az_theme_get() will ever
 * cache. Directories are scanned in readdir() order, which on ext2 tends to
 * follow directory-entry (i.e. creation) order rather than being sorted, so
 * the shipped defaults are named "NN-name.theme" — the leading index is only
 * a hint for humans skimming `ls`, callers must not assume scan order. */
#define AZ_THEME_MAX             32

/* Runtime theme palette. Field layout matches the color roles every DE app
 * already draws with (crust/mantle/base backgrounds, surfaceN panels,
 * overlayN borders, text, an accent pair, and the four semantic status
 * colors) so a *.theme file is just those roles spelled out as hex. */
typedef struct {
    char     name[32];
    uint32_t crust, mantle, base;
    uint32_t surface0, surface1, surface2;
    uint32_t overlay0, overlay1;
    uint32_t text;
    uint32_t accent, accent_sec;
    uint32_t red, green, yellow, blue;
} az_theme_t;

/**
 * Scan /usr/share/themes then /hdd/themes for *.theme files, parse each one
 * and fill out_list (capacity max_themes). Returns the number found (0 if
 * neither directory exists or is empty — callers fall back to compiled-in
 * defaults via az_theme_get(), never to a partially-filled list).
 *
 * Stateless: rescans the directories on every call. Prefer az_theme_count()
 * / az_theme_get() for hot paths — they cache the result per process.
 */
int az_theme_scan_dirs(az_theme_t *out_list, int max_themes);

/**
 * Invalidate the process-local theme cache so the next az_theme_count() or
 * az_theme_get() call rescans disk. Call after installing/editing a theme
 * file (e.g. from the Settings app) in the same process that needs to see it.
 */
void az_theme_reload(void);

/**
 * Cached theme count for this process (scans disk once, on first call).
 * Guaranteed >= 1: if no *.theme files are found on disk, the 5 built-in
 * defaults (Catppuccin Mocha/Latte, Nord, Cyberpunk, OLED) are used so a
 * fresh/incomplete rootfs still has a working theme.
 */
int az_theme_count(void);

/**
 * Cached lookup by index (0 .. az_theme_count()-1). Out-of-range indices
 * clamp to 0 rather than returning NULL — callers that index a palette
 * directly from an untrusted/legacy theme_id never need a NULL check.
 */
const az_theme_t *az_theme_get(unsigned int theme_id);

#ifdef __cplusplus
}
#endif
