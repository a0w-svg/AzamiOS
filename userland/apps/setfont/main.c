/* ============================================================================
 * AzamiOS — Font Management & Configuration Tool (setfont)
 * File: userland/apps/setfont/main.c
 *
 * Provides command-line font discovery, inspection, ASCII character sheet
 * preview, and persistent active system font configuration.
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if __has_include(<azami/font.h>)
#include <azami/font.h>
#elif __has_include("../shared/az_font.h")
#include "../shared/az_font.h"
#elif __has_include("../../libc/include/azami/font.h")
#include "../../libc/include/azami/font.h"
#endif

static void print_usage(const char *prog)
{
    printf("AzamiOS Font Subsystem v1.0 (setfont)\n");
    printf("Usage:\n");
    printf("  %s                   List installed fonts and show active system font\n", prog);
    printf("  %s -l, --list        List all fonts on drive partitions\n", prog);
    printf("  %s -i <path|name>    Inspect font header metadata\n", prog);
    printf("  %s -p <path|name>    Preview font glyphs in terminal\n", prog);
    printf("  %s <path|name>       Set active default system font\n", prog);
    printf("  %s -h, --help        Show this help message\n", prog);
}

static void list_fonts(void)
{
    az_font_info_t list[64];
    int count = az_font_scan_dirs(list, 64);

    az_font_t *cur = az_font_load_default();
    printf("====================================================================\n");
    printf(" AzamiOS Installed Fonts (/usr/share/fonts, /hdd/fonts)\n");
    printf("====================================================================\n");
    if (cur) {
        printf(" Active System Font: %s %s (%dx%d) [%s]\n\n",
               cur->name, cur->style, cur->glyph_w, cur->glyph_h, cur->path);
        az_font_free(cur);
    }

    if (count == 0) {
        printf(" No font files found on drive.\n");
        return;
    }

    printf("  %-20s %-10s %-8s %-10s %s\n", "FAMILY", "STYLE", "SIZE", "BYTES", "PATH");
    printf("  -------------------- ---------- -------- ---------- ------------------------------\n");
    for (int i = 0; i < count; i++) {
        char dims[16];
        snprintf(dims, sizeof(dims), "%dx%d", list[i].glyph_w, list[i].glyph_h);
        printf("  %-20s %-10s %-8s %-10zu %s\n",
               list[i].name, list[i].style, dims, list[i].file_size, list[i].path);
    }
    printf("====================================================================\n");
    printf(" Total: %d font(s) available on drive\n", count);
}

static int inspect_font(const char *target)
{
    az_font_t *font = az_font_load_by_name(target);
    if (!font) {
        fprintf(stderr, "setfont: could not load font '%s'\n", target);
        return 1;
    }

    printf("====================================================================\n");
    printf(" Font Metadata: %s\n", font->path);
    printf("====================================================================\n");
    printf("  Family Name:       %s\n", font->name);
    printf("  Style:             %s\n", font->style);
    printf("  Dimensions:        %d x %d pixels\n", font->glyph_w, font->glyph_h);
    printf("  Character Range:   0x%02X ('%c') .. 0x%02X ('%c')\n",
           font->first_char, (font->first_char >= 0x20 && font->first_char <= 0x7E) ? font->first_char : ' ',
           font->last_char, (font->last_char >= 0x20 && font->last_char <= 0x7E) ? font->last_char : ' ');
    printf("  Glyph Count:       %u\n", font->num_glyphs);
    printf("  Bytes Per Glyph:   %u\n", font->bytes_per_glyph);
    printf("  Flags:             0x%04X (%s%s%s%s)\n",
           font->flags,
           (font->flags & AZF_FLAG_MONO) ? "Monospace " : "",
           (font->flags & AZF_FLAG_BOLD) ? "Bold " : "",
           (font->flags & AZF_FLAG_ITALIC) ? "Italic " : "",
           (font->flags & AZF_FLAG_SERIF) ? "Serif " : "");
    printf("  Storage:           %s\n", font->is_heap ? "Dynamically Loaded from Disk" : "Built-in Memory");
    printf("====================================================================\n");

    az_font_free(font);
    return 0;
}

static int preview_font(const char *target)
{
    az_font_t *font = az_font_load_by_name(target);
    if (!font) {
        fprintf(stderr, "setfont: could not load font '%s'\n", target);
        return 1;
    }

    printf("====================================================================\n");
    printf(" Font Specimen Preview: %s (%dx%d)\n", font->name, font->glyph_w, font->glyph_h);
    printf("====================================================================\n\n");

    const char *sample_chars = "AZAMI 0123456789";
    int row_stride = (font->glyph_w + 7) / 8;

    /* Render sample banner row by row */
    for (int r = 0; r < font->glyph_h; r++) {
        printf("  ");
        for (int i = 0; sample_chars[i]; i++) {
            const unsigned char *g = az_font_get_glyph(font, sample_chars[i]);
            unsigned char bits = g[r * row_stride];
            for (int b = 0; b < font->glyph_w; b++) {
                if (bits & (0x80 >> b)) {
                    printf("##");
                } else {
                    printf("..");
                }
            }
            printf(" ");
        }
        printf("\n");
    }

    printf("\n Specimen Text:\n");
    printf("   \"The quick brown fox jumps over the lazy dog.\"\n");
    printf("   \"0123456789 -- != == <= >= && || -> { } [ ] ( )\"\n");
    printf("====================================================================\n");

    az_font_free(font);
    return 0;
}

static int apply_font(const char *target)
{
    az_font_t *font = az_font_load_by_name(target);
    if (!font) {
        fprintf(stderr, "setfont: font '%s' not found or invalid format\n", target);
        return 1;
    }

    if (az_font_set_default_font(font->path) != 0) {
        fprintf(stderr, "setfont: failed to write /etc/font.conf\n");
        az_font_free(font);
        return 1;
    }

    printf("Applied active system font: %s %s (%dx%d)\n",
           font->name, font->style, font->glyph_w, font->glyph_h);
    printf("Configured in %s and %s\n", AZ_FONT_CONFIG_FILE, AZ_FONT_CONFIG_HDD);

    az_font_free(font);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        list_fonts();
        return 0;
    }

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    if (strcmp(argv[1], "-l") == 0 || strcmp(argv[1], "--list") == 0) {
        list_fonts();
        return 0;
    }

    if (strcmp(argv[1], "-i") == 0 || strcmp(argv[1], "--info") == 0) {
        if (argc < 3) {
            fprintf(stderr, "setfont: missing font name or path for -i\n");
            return 1;
        }
        return inspect_font(argv[2]);
    }

    if (strcmp(argv[1], "-p") == 0 || strcmp(argv[1], "--preview") == 0) {
        if (argc < 3) {
            fprintf(stderr, "setfont: missing font name or path for -p\n");
            return 1;
        }
        return preview_font(argv[2]);
    }

    /* Argument is font name or path to set */
    return apply_font(argv[1]);
}
