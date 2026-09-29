/* ============================================================================
 * AzamiOS — Kernel command line
 * File: kernel/cmdline.c
 *
 * See cmdline.h for the syntax and the parameters the kernel understands.
 *
 * The string comes from Limine's kernel-file response (`kernel_cmdline:` in
 * limine.conf). It lives in bootloader-reclaimable memory, so cmdline_init()
 * copies it before the PMM can recycle that memory; everything afterwards
 * reads the copy, which becomes read-only when kprotect_seal() runs.
 *
 * Lookups rescan the string each time. It is at most CMDLINE_MAX bytes, the
 * handful of callers all run once during boot, and a scan keeps the one
 * source of truth (the string /proc/cmdline shows) as the only state.
 * ============================================================================ */

#include "cmdline.h"
#include "../include/azami/sections.h"
#include "../arch/x86_64/boot/limine_req.h"
#include "lib/string.h"

static char g_cmdline[CMDLINE_MAX] __ro_after_init;

void cmdline_init(void)
{
    struct limine_kernel_file_response *r =
        (struct limine_kernel_file_response *)g_limine_kfile_req.response;
    const char *src = (r && r->kernel_file) ? r->kernel_file->cmdline : NULL;
    if (!src) {
        g_cmdline[0] = '\0';
        return;
    }

    /* Copy, turning tabs/newlines into spaces so a multi-line bootloader
     * entry parses the way it reads, and trimming leading/trailing blanks. */
    while (*src == ' ' || *src == '\t' || *src == '\n' || *src == '\r') src++;
    size_t n = 0;
    for (; src[n] && n < CMDLINE_MAX - 1; n++) {
        char c = src[n];
        g_cmdline[n] = (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
    }
    while (n > 0 && g_cmdline[n - 1] == ' ') n--;
    g_cmdline[n] = '\0';
}

const char *cmdline_get(void)
{
    return g_cmdline;
}

/* ── Tokenizer ────────────────────────────────────────────────────────────── */

typedef struct {
    const char *key;   size_t key_len;
    const char *val;   size_t val_len;   /* val == NULL: bare flag */
} cmdline_param_t;

/* Parse the parameter starting at *pos; advance *pos past it. Returns false at
 * end of string. Quotes may enclose the value or the whole parameter
 * ("key=a b"), and are not part of what is returned. */
static bool next_param(const char **pos, cmdline_param_t *p)
{
    const char *s = *pos;
    while (*s == ' ') s++;
    if (!*s) { *pos = s; return false; }

    bool quoted = false;
    if (*s == '"') { quoted = true; s++; }

    p->key = s;
    while (*s && *s != '=' && (quoted ? *s != '"' : *s != ' ')) s++;
    p->key_len = (size_t)(s - p->key);
    p->val = NULL;
    p->val_len = 0;

    if (*s == '=') {
        s++;
        if (!quoted && *s == '"') { quoted = true; s++; }
        p->val = s;
        while (*s && (quoted ? *s != '"' : *s != ' ')) s++;
        p->val_len = (size_t)(s - p->val);
    }
    if (quoted && *s == '"') s++;
    /* Anything glued to a closing quote belongs to this parameter. */
    while (*s && *s != ' ') s++;

    *pos = s;
    return true;
}

static bool key_matches(const cmdline_param_t *p, const char *key)
{
    size_t kl = strlen(key);
    return p->key_len == kl && memcmp(p->key, key, kl) == 0;
}

/* The last occurrence of @key, which is the one that counts. */
static bool find_last(const char *key, cmdline_param_t *out)
{
    const char *pos = g_cmdline;
    cmdline_param_t p;
    bool found = false;
    while (next_param(&pos, &p)) {
        if (key_matches(&p, key)) { *out = p; found = true; }
    }
    return found;
}

/* ── Public lookups ───────────────────────────────────────────────────────── */

bool cmdline_has(const char *key)
{
    cmdline_param_t p;
    return find_last(key, &p);
}

bool cmdline_get_str(const char *key, char *out, size_t len)
{
    cmdline_param_t p;
    if (!out || len == 0 || !find_last(key, &p) || !p.val) return false;
    size_t n = p.val_len < len - 1 ? p.val_len : len - 1;
    memcpy(out, p.val, n);
    out[n] = '\0';
    return true;
}

bool cmdline_get_bool(const char *key, bool def)
{
    cmdline_param_t p;
    if (!find_last(key, &p)) return def;
    if (!p.val) return true;

    static const char *const yes[] = { "1", "y", "yes", "on", "true" };
    static const char *const no[]  = { "0", "n", "no", "off", "false" };
    char v[8];
    if (p.val_len == 0 || p.val_len >= sizeof(v)) return def;
    memcpy(v, p.val, p.val_len);
    v[p.val_len] = '\0';
    for (size_t i = 0; i < sizeof(yes) / sizeof(yes[0]); i++)
        if (strcasecmp(v, yes[i]) == 0) return true;
    for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++)
        if (strcasecmp(v, no[i]) == 0) return false;
    return def;
}

long cmdline_get_long(const char *key, long def)
{
    cmdline_param_t p;
    if (!find_last(key, &p) || !p.val || p.val_len == 0) return def;

    const char *s = p.val, *end = p.val + p.val_len;
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    unsigned base = 10;
    if (end - s > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    if (s == end) return def;

    long v = 0;
    for (; s < end; s++) {
        unsigned d;
        if (*s >= '0' && *s <= '9')                   d = (unsigned)(*s - '0');
        else if (base == 16 && *s >= 'a' && *s <= 'f') d = (unsigned)(*s - 'a' + 10);
        else if (base == 16 && *s >= 'A' && *s <= 'F') d = (unsigned)(*s - 'A' + 10);
        else return def;
        if (v > (__LONG_MAX__ - (long)d) / (long)base) return def;   /* overflow */
        v = v * (long)base + (long)d;
    }
    return neg ? -v : v;
}

int cmdline_for_each(const char *key, void (*fn)(const char *val, void *ctx), void *ctx)
{
    const char *pos = g_cmdline;
    cmdline_param_t p;
    int count = 0;
    while (next_param(&pos, &p)) {
        if (!key_matches(&p, key) || !p.val) continue;
        char v[64];
        size_t n = p.val_len < sizeof(v) - 1 ? p.val_len : sizeof(v) - 1;
        memcpy(v, p.val, n);
        v[n] = '\0';
        if (fn) fn(v, ctx);
        count++;
    }
    return count;
}
