/* ============================================================================
 * AzamiOS Userspace — Package Manager (pkg.elf)
 * File: userland/apps/pkg/main.c
 *
 * `pkg install [--force] <name>...` / `pkg remove <name>...` /
 * `pkg list [--available]` / `pkg search <term>` / `pkg info <name>` /
 * `pkg repo list|add|remove`.
 *
 * A package is a plain ustar archive (the same format tar.elf reads and
 * writes) whose first entry is named "PKGINFO" — a small key=value
 * manifest (name/version/type/description) — followed by the files to
 * install, with paths relative to the root filesystem ("bin/cowsay.elf",
 * not "/bin/cowsay.elf"). `type` is "native" for an AzamiOS-toolchain ELF
 * or "linux" for a stock Linux binary (busybox and friends); the manager
 * itself does not care which — both run under this kernel's Linux-ABI
 * syscall layer, so installing either is exactly "copy the files, remember
 * what got copied".
 *
 * A repository is a directory — local (file:///path) or served over plain
 * HTTP (http://host[:port]/path) — holding an index.txt (one
 * "name version type file size sha256 description..." line per package,
 * whitespace-separated for the first six fields, description running to
 * end of line) and the package archives it names. The size and digest
 * columns are optional: an index whose fifth field is not a byte count
 * followed by a 64-hex-digit digest is read as the original four-field
 * format, with the description starting right after the filename — so a
 * repository published before those columns existed still installs, just
 * without the integrity check. /etc/pkg/repos.conf lists one repository
 * URL per line, checked in order until a package name matches; `pkg repo
 * add/remove` edit that file rather than making you do it by hand.
 *
 * Anything fetched from a repository that *does* carry a digest is hashed
 * before a single byte of it is unpacked — a package archive arriving over
 * http:// crosses a network this OS does not control, and "extract first,
 * notice later" is not a thing a package manager should do.
 *
 * Installed-package state lives under /var/pkg/db/<name>: the manifest
 * again, then a "FILES:" line, then one installed path per line — exactly
 * what `pkg remove` deletes and `pkg list` reads back.
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
#include <stdint.h>
#include <ctype.h>
#include <sha256.h>

#define REPOS_CONF   "/etc/pkg/repos.conf"
#define DB_DIR       "/var/pkg/db"
#define CACHE_DIR    "/var/pkg/cache"
#define MAX_REPOS    16
#define MAX_LINE     512

/* ── ustar reader (mirrors tar.elf's struct and octal-field parsing — see
 * userland/apps/tar/main.c; kept as its own copy rather than a shared
 * helper so this tool stays a single self-contained static binary, same as
 * every other app here) ─────────────────────────────────────────────── */
struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};

static unsigned long parse_octal(const char *p, size_t len)
{
    unsigned long val = 0;
    while (len > 0 && (*p == ' ' || *p == '\0')) { p++; len--; }
    while (len > 0 && *p >= '0' && *p <= '7') { val = (val << 3) + (unsigned long)(*p - '0'); p++; len--; }
    return val;
}

struct manifest {
    char name[64];
    char version[32];
    char type[16];        /* "native" or "linux" */
    char description[192];
};

static void manifest_clear(struct manifest *m) { memset(m, 0, sizeof(*m)); }

/* Parses one "key=value" line into the manifest; unknown keys are ignored
 * so PKGINFO can grow fields later without breaking older pkg binaries. */
static void manifest_set(struct manifest *m, const char *key, const char *val)
{
    if (strcmp(key, "name") == 0)        snprintf(m->name, sizeof(m->name), "%s", val);
    else if (strcmp(key, "version") == 0) snprintf(m->version, sizeof(m->version), "%s", val);
    else if (strcmp(key, "type") == 0)    snprintf(m->type, sizeof(m->type), "%s", val);
    else if (strcmp(key, "description") == 0) snprintf(m->description, sizeof(m->description), "%s", val);
}

static void manifest_parse_buf(struct manifest *m, char *buf, size_t len)
{
    char *line = buf;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || buf[i] == '\n') {
            buf[i] = '\0';
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                manifest_set(m, line, eq + 1);
            }
            line = buf + i + 1;
        }
    }
}

/* mkdir -p: creates every missing component of `path` itself (not its
 * parent). Existing components are left alone — EEXIST is the expected
 * case, not an error.
 *
 * This used to be a single non-recursive mkdir of the parent, which was
 * enough only while every package installed into bin/ or sbin/, directories
 * the base image already has. Packages now ship into paths several levels
 * deep that the image has never seen (usr/share/doc/azami/,
 * usr/share/examples/), where one mkdir of the leaf parent fails with
 * ENOENT and every file in the package then silently fails to open. */
static void mkdir_p(const char *path)
{
    char buf[300];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *q = buf + 1; *q; q++) {
        if (*q != '/') continue;
        *q = '\0';
        mkdir(buf, 0755);
        *q = '/';
    }
    mkdir(buf, 0755);
}

/* Ensures the directory holding `path` exists, creating the whole chain. */
static void ensure_parent_dir(const char *path)
{
    char buf[300];
    snprintf(buf, sizeof(buf), "%s", path);
    char *slash = strrchr(buf, '/');
    if (!slash || slash == buf) return;
    *slash = '\0';
    mkdir_p(buf);
}

/* Extracts a package tar at `tarpath` into the root filesystem, splitting
 * off the PKGINFO manifest entry and recording every other file's install
 * path into `files_out` (newline-separated) so `pkg remove` can undo
 * exactly what this installed — nothing more, nothing less. Returns 0 on
 * success. */
static int extract_package(const char *tarpath, struct manifest *m, FILE *files_out)
{
    int fd = open(tarpath, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "pkg: cannot open %s: %s\n", tarpath, strerror(errno)); return -1; }

    manifest_clear(m);
    struct tar_header h;
    int saw_manifest = 0;

    while (read(fd, &h, sizeof(h)) == (ssize_t)sizeof(h)) {
        if (h.name[0] == '\0') break;
        unsigned long size = parse_octal(h.size, 12);
        unsigned long blocks = (size + 511) / 512;

        if (strcmp(h.name, "PKGINFO") == 0) {
            char *buf = (char *)malloc(size + 1);
            if (buf) {
                size_t got = 0;
                while (got < size) {
                    ssize_t n = read(fd, buf + got, size - got);
                    if (n <= 0) break;
                    got += (size_t)n;
                }
                /* Skip the padding to the next 512-byte boundary. */
                lseek(fd, (off_t)(blocks * 512 - got), SEEK_CUR);
                manifest_parse_buf(m, buf, got);
                free(buf);
                saw_manifest = 1;
            }
            continue;
        }

        if (h.typeflag == '5' || (h.name[strlen(h.name) - 1] == '/')) {
            mkdir_p(h.name);
            continue;
        }

        /* Symlink entry: no data blocks follow it, only the header's
         * linkname. This is what lets a toolbox package (busybox, toybox)
         * ship one binary plus the several hundred applet names that
         * dispatch to it — without this, each of those entries became an
         * empty regular file and `grep` on the PATH was a zero-byte
         * nothing. An existing name is replaced, same as a regular file
         * would be. */
        if (h.typeflag == '2') {
            char target[101];
            memcpy(target, h.linkname, sizeof(h.linkname));
            target[sizeof(h.linkname)] = '\0';
            ensure_parent_dir(h.name);
            unlink(h.name);
            if (symlink(target, h.name) != 0) {
                fprintf(stderr, "pkg: cannot link %s -> %s: %s\n",
                        h.name, target, strerror(errno));
                continue;
            }
            if (files_out) fprintf(files_out, "%s\n", h.name);
            continue;
        }

        ensure_parent_dir(h.name);
        int out_fd = open(h.name, O_WRONLY | O_CREAT | O_TRUNC, 0755);
        if (out_fd < 0) {
            fprintf(stderr, "pkg: cannot create %s: %s\n", h.name, strerror(errno));
            lseek(fd, (off_t)(blocks * 512), SEEK_CUR);
            continue;
        }
        unsigned long remaining = size;
        char buf[4096];
        while (remaining > 0) {
            size_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
            size_t padded = ((chunk + 511) / 512) * 512;
            ssize_t n = read(fd, buf, padded < sizeof(buf) ? padded : sizeof(buf));
            if (n <= 0) break;
            size_t usable = (size_t)n < chunk ? (size_t)n : chunk;
            write(out_fd, buf, usable);
            remaining -= usable;
        }
        close(out_fd);
        if (files_out) fprintf(files_out, "%s\n", h.name);
    }

    close(fd);
    if (!saw_manifest) {
        fprintf(stderr, "pkg: %s has no PKGINFO — not a package archive\n", tarpath);
        return -1;
    }
    return 0;
}

/* ── Minimal HTTP GET (same approach as curl.elf: resolve, connect, send a
 * bare request, copy the body — chunked transfer-encoding is out of scope
 * here just as it is there) ─────────────────────────────────────────── */

/* A no-op handler, not SIG_IGN: the point is only to make alarm()'s
 * SIGALRM *interrupt* a blocked connect()/recv() with -EINTR (which a
 * default-terminate or ignored disposition would not do) rather than kill
 * the process — a repo whose network is unreachable should fail the one
 * install/list, not hang pkg.elf forever, and not exit it either.
 *
 * Installed via sigaction(), not the signal() wrapper: this libc's
 * signal() always sets SA_RESTART (userland/libc/signal.c), which makes
 * the kernel transparently re-issue the interrupted connect()/recv() once
 * the handler returns — exactly the automatic-retry behaviour a timeout
 * exists to avoid. sigaction() with sa_flags left at 0 is what actually
 * lets the blocked call come back with -EINTR instead of quietly
 * restarting, which is what turns a 15-second alarm into an actual
 * timeout instead of a no-op. */
static void alarm_noop(int sig) { (void)sig; }

static int http_get(const char *host, int port, const char *path, int out_fd)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = alarm_noop;
    sigaction(SIGALRM, &sa, NULL);
    alarm(15);

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        fprintf(stderr, "pkg: could not resolve %s\n", host);
        alarm(0); return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0 || connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        fprintf(stderr, "pkg: could not connect to %s:%d: %s\n", host, port, strerror(errno));
        if (fd >= 0) close(fd);
        freeaddrinfo(res);
        alarm(0); return -1;
    }
    freeaddrinfo(res);

    char req[512];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: AzamiOS-pkg/1.0\r\n"
        "Accept: */*\r\nConnection: close\r\n\r\n", path, host);
    if (send(fd, req, (size_t)req_len, 0) < 0) { close(fd); alarm(0); return -1; }

    char buf[4096];
    int header_done = 0;
    ssize_t n;
    ssize_t total = 0;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        char *p = buf;
        size_t len = (size_t)n;
        if (!header_done) {
            char tmp[4096 + 1];
            memcpy(tmp, buf, len);
            tmp[len] = '\0';
            char *end = strstr(tmp, "\r\n\r\n");
            if (!end) continue; /* still inside headers, discard this chunk */
            if (strstr(tmp, " 200 ") == NULL && strstr(tmp, "HTTP/1.") == tmp) {
                fprintf(stderr, "pkg: HTTP error fetching %s%s\n", host, path);
                close(fd);
                alarm(0); return -1;
            }
            header_done = 1;
            size_t hlen = (size_t)(end - tmp) + 4;
            p = buf + hlen;
            len -= hlen;
        }
        if (len > 0) { write(out_fd, p, len); total += (ssize_t)len; }
    }
    close(fd);
    alarm(0); return header_done ? (int)total : -1;
}

/* ── Repository URL handling ─────────────────────────────────────────── */
struct repo {
    int is_http;
    char host[128];
    int port;
    char path[256];   /* for http://: the URL path prefix. for file://: the local dir. */
    char url[256];    /* the line from repos.conf, verbatim, for messages */
};

static int parse_repo_url(const char *url, struct repo *r)
{
    memset(r, 0, sizeof(*r));
    snprintf(r->url, sizeof(r->url), "%s", url);
    if (strncmp(url, "file://", 7) == 0) {
        r->is_http = 0;
        snprintf(r->path, sizeof(r->path), "%s", url + 7);
        size_t l = strlen(r->path);
        while (l > 1 && r->path[l - 1] == '/') { r->path[--l] = '\0'; }
        return 0;
    }
    if (strncmp(url, "http://", 7) == 0) {
        r->is_http = 1;
        r->port = 80;
        const char *p = url + 7;
        const char *slash = strchr(p, '/');
        const char *colon = strchr(p, ':');
        size_t hostlen = slash ? (size_t)(slash - p) : strlen(p);
        if (colon && (!slash || colon < slash)) hostlen = (size_t)(colon - p);
        if (hostlen >= sizeof(r->host)) hostlen = sizeof(r->host) - 1;
        memcpy(r->host, p, hostlen);
        r->host[hostlen] = '\0';
        if (colon && (!slash || colon < slash)) r->port = atoi(colon + 1);
        snprintf(r->path, sizeof(r->path), "%s", slash ? slash : "");
        size_t l = strlen(r->path);
        while (l > 0 && r->path[l - 1] == '/') { r->path[--l] = '\0'; }
        /* The prefix is stored without its trailing slash, including for a
         * bare "http://host" (path ""), because repo_fetch() builds
         * "<prefix>/<file>" itself -- leaving the "/" on would ask for
         * "//index.txt". Lenient servers normalize that away; not every
         * server is lenient. */
        return 0;
    }
    return -1;
}

/* Fetches `repo`'s file at `name` (index.txt, or a package archive) into
 * a freshly created file at `dest_path`. Returns 0 on success. */
static int repo_fetch(const struct repo *repo, const char *name, const char *dest_path)
{
    if (!repo->is_http) {
        char src[512];
        snprintf(src, sizeof(src), "%s/%s", repo->path, name);
        int in_fd = open(src, O_RDONLY);
        if (in_fd < 0) return -1;
        int out_fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out_fd < 0) { close(in_fd); return -1; }
        char buf[8192];
        ssize_t n;
        while ((n = read(in_fd, buf, sizeof(buf))) > 0) write(out_fd, buf, (size_t)n);
        close(in_fd);
        close(out_fd);
        return 0;
    }

    /* A `pkg install` needs two HTTP fetches — the repo's index, then the
     * package archive — and for a while only the *first* TCP connection a
     * boot made completed; a second one hung in the kernel with no SYN on
     * the wire. That was a locking bug in tcp_connect()/tcp_input() (see
     * kernel/net/tcp.c), not anything about this tool, and is fixed:
     * back-to-back fetches within one boot work. See docs/PACKAGES.md for
     * how to serve a repository over http:// (scripts/serve_pkg_repo.py,
     * `make pkgserve`). */
    char urlpath[512];
    snprintf(urlpath, sizeof(urlpath), "%s/%s", repo->path, name);
    int out_fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) return -1;
    int rc = http_get(repo->host, repo->port, urlpath, out_fd);
    close(out_fd);
    if (rc < 0) { unlink(dest_path); return -1; }
    return 0;
}

static int load_repos(struct repo repos[MAX_REPOS])
{
    FILE *f = fopen(REPOS_CONF, "r");
    if (!f) return 0;
    int n = 0;
    char line[MAX_LINE];
    while (n < MAX_REPOS && fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        if (l == 0 || line[0] == '#') continue;
        if (parse_repo_url(line, &repos[n]) == 0) n++;
    }
    fclose(f);
    return n;
}

/* One index.txt line:
 *
 *     name version type file size sha256 description...
 *
 * The first six fields are whitespace-separated; the description is
 * everything after them, taken verbatim (so it may itself contain spaces).
 *
 * `size` and `sha256` are optional. A repository generated before those
 * columns existed writes only four fields and starts the description at
 * the fifth, so they are recognised by shape rather than by position: the
 * fifth field counts as a size only if it is all digits *and* the sixth is
 * exactly 64 hex characters. A description beginning with a number
 * therefore still parses as a description, because no plausible next word
 * is a 64-character hex string. */
struct index_entry {
    char name[64], version[32], type[16], file[128], description[192];
    unsigned long size;    /* archive size in bytes, 0 if the index omits it */
    char sha256[65];       /* archive digest, "" if the index omits it */
};

static int field_is_digits(const char *p, size_t n)
{
    if (n == 0 || n > 20) return 0;
    for (size_t i = 0; i < n; i++) if (p[i] < '0' || p[i] > '9') return 0;
    return 1;
}

static int field_is_hex64(const char *p, size_t n)
{
    if (n != 64) return 0;
    for (size_t i = 0; i < 64; i++) if (!isxdigit((unsigned char)p[i])) return 0;
    return 1;
}

static void field_copy(char *dst, size_t dstsz, const char *src, size_t n)
{
    if (n >= dstsz) n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int parse_index_line(const char *line, struct index_entry *e)
{
    memset(e, 0, sizeof(*e));

    const char *tok[6];
    size_t toklen[6];
    const char *after[6];   /* where each token ends, for the description */
    int nt = 0;
    const char *p = line;
    while (nt < 6) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        tok[nt] = start;
        toklen[nt] = (size_t)(p - start);
        after[nt] = p;
        nt++;
    }
    if (nt < 4) return -1;

    field_copy(e->name,    sizeof(e->name),    tok[0], toklen[0]);
    field_copy(e->version, sizeof(e->version), tok[1], toklen[1]);
    field_copy(e->type,    sizeof(e->type),    tok[2], toklen[2]);
    field_copy(e->file,    sizeof(e->file),    tok[3], toklen[3]);

    const char *rest = after[3];
    if (nt == 6 && field_is_digits(tok[4], toklen[4]) && field_is_hex64(tok[5], toklen[5])) {
        char sizebuf[24];
        field_copy(sizebuf, sizeof(sizebuf), tok[4], toklen[4]);
        e->size = strtoul(sizebuf, NULL, 10);
        field_copy(e->sha256, sizeof(e->sha256), tok[5], toklen[5]);
        for (char *h = e->sha256; *h; h++) *h = (char)tolower((unsigned char)*h);
        rest = after[5];
    }

    while (*rest == ' ' || *rest == '\t') rest++;
    snprintf(e->description, sizeof(e->description), "%s", rest);
    return 0;
}

/* "1.1M", "94K", "812B" — a size column narrow enough to sit in `pkg list
 * --available` without pushing the description off an 80-column serial
 * console. */
static void human_size(unsigned long bytes, char *out, size_t outsz)
{
    if (bytes == 0)            snprintf(out, outsz, "-");
    else if (bytes < 1024)     snprintf(out, outsz, "%luB", bytes);
    else if (bytes < 1024UL * 1024) snprintf(out, outsz, "%luK", (bytes + 512) / 1024);
    else                       snprintf(out, outsz, "%lu.%luM", bytes / (1024UL * 1024),
                                        ((bytes % (1024UL * 1024)) * 10) / (1024UL * 1024));
}

/* Checks a fetched archive against the index's size and digest before
 * anything is unpacked from it. Returns 0 if it matches or if the index
 * carries no digest to match against (the old four-field format), -1 on a
 * mismatch — in which case the caller must not install it. */
static int verify_archive(const char *path, const struct index_entry *e)
{
    if (!e->sha256[0]) return 0;

    if (e->size) {
        struct stat st;
        if (stat(path, &st) == 0 && (unsigned long)st.st_size != e->size) {
            fprintf(stderr, "pkg: %s is %lu bytes, index says %lu — refusing to install\n",
                    e->file, (unsigned long)st.st_size, e->size);
            return -1;
        }
    }

    char got[65];
    if (sha256_hash_file(path, got) != 0) {
        fprintf(stderr, "pkg: cannot hash %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (strcmp(got, e->sha256) != 0) {
        fprintf(stderr, "pkg: checksum mismatch for %s — refusing to install\n", e->file);
        fprintf(stderr, "pkg:   expected %s\n", e->sha256);
        fprintf(stderr, "pkg:   got      %s\n", got);
        return -1;
    }
    return 0;
}

/* Walks every configured repository's index, handing each parsed entry to
 * `cb` along with the repository it came from. A callback returning
 * non-zero stops the walk immediately (that is how a lookup short-circuits
 * once it has its match, instead of downloading the remaining indexes for
 * nothing); the walk's return value is that value, or 0 if it ran to the
 * end.
 *
 * install/list/search/info all differ only in what they do per entry, and
 * each used to carry its own copy of "mkstemp, fetch index.txt, fgets,
 * strip CR/LF, skip blanks and #comments, parse, unlink" — four copies of
 * the same twenty lines, of which two had already drifted apart. */
typedef int (*index_cb)(const struct index_entry *e, const struct repo *repo, void *ctx);

static int for_each_index_entry(struct repo repos[MAX_REPOS], int nrepos, index_cb cb, void *ctx)
{
    int rc = 0;
    for (int i = 0; i < nrepos && rc == 0; i++) {
        char idx_path[] = "/tmp/pkg-index-XXXXXX";
        int fd = mkstemp(idx_path);
        if (fd < 0) continue;
        close(fd);
        if (repo_fetch(&repos[i], "index.txt", idx_path) != 0) {
            fprintf(stderr, "pkg: warning: cannot read index of %s\n", repos[i].url);
            unlink(idx_path);
            continue;
        }

        FILE *f = fopen(idx_path, "r");
        if (f) {
            char line[MAX_LINE];
            while (fgets(line, sizeof(line), f)) {
                size_t l = strlen(line);
                while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
                if (l == 0 || line[0] == '#') continue;
                struct index_entry e;
                if (parse_index_line(line, &e) != 0) continue;
                rc = cb(&e, &repos[i], ctx);
                if (rc != 0) break;
            }
            fclose(f);
        }
        unlink(idx_path);
    }
    return rc;
}

struct find_ctx {
    const char *name;
    struct index_entry entry;
    struct repo repo;
};

static int find_cb(const struct index_entry *e, const struct repo *repo, void *ctx)
{
    struct find_ctx *fc = (struct find_ctx *)ctx;
    if (strcmp(e->name, fc->name) != 0) return 0;
    fc->entry = *e;
    fc->repo  = *repo;
    return 1;   /* stop: first repository to offer the name wins */
}

/* Searches every configured repo's index for `name`; on a match, fills
 * `entry` and `*found_repo` and returns 0. */
static int find_in_repos(const char *name, struct repo repos[MAX_REPOS], int nrepos,
                          struct index_entry *entry, struct repo *found_repo)
{
    struct find_ctx fc;
    memset(&fc, 0, sizeof(fc));
    fc.name = name;
    if (for_each_index_entry(repos, nrepos, find_cb, &fc) != 1) return -1;
    *entry = fc.entry;
    *found_repo = fc.repo;
    return 0;
}

static int is_installed(const char *name)
{
    char dbpath[256];
    snprintf(dbpath, sizeof(dbpath), "%s/%s", DB_DIR, name);
    struct stat st;
    return stat(dbpath, &st) == 0;
}

/* ── Installed-package database (/var/pkg/db/<name>) ────────────────── */
static int db_write(const struct manifest *m, const char *files_tmp_path)
{
    mkdir_p(DB_DIR);
    char dbpath[256];
    snprintf(dbpath, sizeof(dbpath), "%s/%s", DB_DIR, m->name);
    FILE *out = fopen(dbpath, "w");
    if (!out) return -1;
    fprintf(out, "name=%s\nversion=%s\ntype=%s\ndescription=%s\nFILES:\n",
            m->name, m->version, m->type, m->description);
    FILE *in = fopen(files_tmp_path, "r");
    if (in) {
        char line[300];
        while (fgets(line, sizeof(line), in)) fputs(line, out);
        fclose(in);
    }
    fclose(out);
    return 0;
}

static int db_read(const char *name, struct manifest *m, char ***files_out, int *nfiles_out)
{
    char dbpath[256];
    snprintf(dbpath, sizeof(dbpath), "%s/%s", DB_DIR, name);
    FILE *f = fopen(dbpath, "r");
    if (!f) return -1;
    manifest_clear(m);
    char **files = NULL;
    int nfiles = 0, cap = 0;
    int in_files = 0;
    char line[300];
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        if (!in_files) {
            if (strcmp(line, "FILES:") == 0) { in_files = 1; continue; }
            char *eq = strchr(line, '=');
            if (eq) { *eq = '\0'; manifest_set(m, line, eq + 1); }
        } else {
            if (l == 0) continue;
            if (nfiles == cap) {
                cap = cap ? cap * 2 : 8;
                files = (char **)realloc(files, (size_t)cap * sizeof(char *));
            }
            files[nfiles++] = strdup(line);
        }
    }
    fclose(f);
    if (files_out) *files_out = files;
    if (nfiles_out) *nfiles_out = nfiles;
    return 0;
}

/* ── Commands ─────────────────────────────────────────────────────────── */

/* Reads a newline-separated file (the just-installed file list) into a
 * malloc'd array of malloc'd strings. Returns NULL on failure. */
static char **read_lines(const char *path, int *count_out)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char **v = NULL;
    int n = 0, cap = 0;
    char line[300];
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        if (l == 0) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            v = (char **)realloc(v, (size_t)cap * sizeof(char *));
        }
        v[n++] = strdup(line);
    }
    fclose(f);
    *count_out = n;
    return v;
}

static void free_lines(char **v, int n)
{
    for (int i = 0; i < n; i++) free(v[i]);
    free(v);
}

static int in_list(char **v, int n, const char *s)
{
    for (int i = 0; i < n; i++) if (strcmp(v[i], s) == 0) return 1;
    return 0;
}

static int cmd_install(const char *name, int force)
{
    if (is_installed(name) && !force) {
        printf("pkg: '%s' is already installed (pkg install --force %s to reinstall)\n", name, name);
        return 0;
    }

    struct repo repos[MAX_REPOS];
    int nrepos = load_repos(repos);
    if (nrepos == 0) {
        fprintf(stderr, "pkg: no repositories configured (%s)\n", REPOS_CONF);
        return 1;
    }

    struct index_entry entry;
    struct repo repo;
    if (find_in_repos(name, repos, nrepos, &entry, &repo) != 0) {
        fprintf(stderr, "pkg: package '%s' not found in any configured repository\n", name);
        return 1;
    }

    mkdir_p(CACHE_DIR);
    char pkg_path[300];
    snprintf(pkg_path, sizeof(pkg_path), "%s/%s", CACHE_DIR, entry.file);
    char sizebuf[16];
    human_size(entry.size, sizebuf, sizeof(sizebuf));
    printf("pkg: fetching %s (%s %s, %s) from %s...\n",
           entry.file, entry.name, entry.version, sizebuf, repo.url);
    if (repo_fetch(&repo, entry.file, pkg_path) != 0) {
        fprintf(stderr, "pkg: failed to fetch %s\n", entry.file);
        return 1;
    }

    /* Verify before unpacking, never after: a mismatch here means the
     * archive is truncated, corrupted in the cache, or not the file the
     * index describes, and in none of those cases should its contents
     * reach the filesystem. The bad copy goes too, so the next attempt
     * re-fetches instead of re-reading the same broken cache entry. */
    if (verify_archive(pkg_path, &entry) != 0) {
        unlink(pkg_path);
        return 1;
    }
    if (entry.sha256[0]) printf("pkg: checksum ok (sha256 %.16s...)\n", entry.sha256);

    /* On a reinstall/upgrade, remember what the previous version owned so
     * files it had and the new one does not can be cleaned up afterwards —
     * otherwise every install-over leaves the old version's orphans behind
     * with nothing recording that they exist. */
    char **old_files = NULL;
    int n_old = 0;
    if (is_installed(name)) {
        struct manifest om;
        db_read(name, &om, &old_files, &n_old);
    }

    char files_tmp[] = "/tmp/pkg-files-XXXXXX";
    int ffd = mkstemp(files_tmp);
    if (ffd < 0) {
        fprintf(stderr, "pkg: mkstemp failed: %s\n", strerror(errno));
        free_lines(old_files, n_old);
        return 1;
    }
    FILE *files_out = fdopen(ffd, "w");

    struct manifest m;
    printf("pkg: installing %s %s...\n", entry.name, entry.version);
    int rc = extract_package(pkg_path, &m, files_out);
    if (files_out) fclose(files_out);

    if (rc == 0) {
        if (!m.name[0]) snprintf(m.name, sizeof(m.name), "%s", entry.name);
        db_write(&m, files_tmp);

        int n_new = 0;
        char **new_files = read_lines(files_tmp, &n_new);
        for (int i = 0; i < n_old; i++) {
            if (new_files && in_list(new_files, n_new, old_files[i])) continue;
            if (unlink(old_files[i]) == 0)
                printf("pkg: removed stale %s\n", old_files[i]);
        }
        free_lines(new_files, n_new);

        printf("pkg: installed %s %s — %s\n", m.name, m.version, m.description);
    } else {
        fprintf(stderr, "pkg: install of '%s' failed\n", name);
    }
    free_lines(old_files, n_old);
    unlink(files_tmp);
    return rc == 0 ? 0 : 1;
}

static int cmd_remove(const char *name)
{
    struct manifest m;
    char **files = NULL;
    int nfiles = 0;
    if (db_read(name, &m, &files, &nfiles) != 0) {
        fprintf(stderr, "pkg: '%s' is not installed\n", name);
        return 1;
    }
    for (int i = 0; i < nfiles; i++) {
        if (unlink(files[i]) == 0) printf("pkg: removed %s\n", files[i]);
        free(files[i]);
    }
    free(files);
    char dbpath[256];
    snprintf(dbpath, sizeof(dbpath), "%s/%s", DB_DIR, name);
    unlink(dbpath);
    printf("pkg: removed %s %s\n", m.name[0] ? m.name : name, m.version);
    return 0;
}

/* One row of `pkg list --available` / `pkg search`: an "i" in the first
 * column for what is already installed, then name, version, type, archive
 * size and description. */
static void print_catalog_row(const struct index_entry *e)
{
    char sizebuf[16];
    human_size(e->size, sizebuf, sizeof(sizebuf));
    printf("%s %-16s %-10s %-7s %-6s %s\n",
           is_installed(e->name) ? "i" : " ",
           e->name, e->version, e->type, sizebuf, e->description);
}

static void print_catalog_header(void)
{
    printf("  %-16s %-10s %-7s %-6s %s\n", "NAME", "VERSION", "TYPE", "SIZE", "DESCRIPTION");
}

struct catalog_ctx {
    const char *term;   /* NULL for "list everything" */
    int matches;
};

static int catalog_cb(const struct index_entry *e, const struct repo *repo, void *ctx)
{
    (void)repo;
    struct catalog_ctx *cc = (struct catalog_ctx *)ctx;
    if (cc->term && !strcasestr(e->name, cc->term) && !strcasestr(e->description, cc->term))
        return 0;
    if (cc->matches == 0) print_catalog_header();
    cc->matches++;
    print_catalog_row(e);
    return 0;
}

static int cmd_list(int available)
{
    if (!available) {
        DIR *d = opendir(DB_DIR);
        if (!d) { printf("(no packages installed)\n"); return 0; }
        struct dirent *de;
        int count = 0;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            struct manifest m;
            if (db_read(de->d_name, &m, NULL, NULL) == 0) {
                if (count == 0) printf("%-16s %-10s %-8s %s\n", "NAME", "VERSION", "TYPE", "DESCRIPTION");
                printf("%-16s %-10s %-8s %s\n", m.name, m.version, m.type, m.description);
                count++;
            }
        }
        closedir(d);
        if (count == 0) printf("(no packages installed)\n");
        return 0;
    }

    struct repo repos[MAX_REPOS];
    int nrepos = load_repos(repos);
    if (nrepos == 0) { fprintf(stderr, "pkg: no repositories configured (%s)\n", REPOS_CONF); return 1; }

    struct catalog_ctx cc = { NULL, 0 };
    for_each_index_entry(repos, nrepos, catalog_cb, &cc);
    if (cc.matches == 0) printf("(no packages available)\n");
    return 0;
}

static int cmd_search(const char *term)
{
    struct repo repos[MAX_REPOS];
    int nrepos = load_repos(repos);
    if (nrepos == 0) { fprintf(stderr, "pkg: no repositories configured (%s)\n", REPOS_CONF); return 1; }

    struct catalog_ctx cc = { term, 0 };
    for_each_index_entry(repos, nrepos, catalog_cb, &cc);
    if (cc.matches == 0) {
        printf("pkg: nothing matching '%s'\n", term);
        return 1;
    }
    return 0;
}

/* `pkg info <name>`: whatever is known about a package from both sides —
 * the installed database (what it put where) and the repositories (what
 * they currently offer). Either half alone is enough to print something
 * useful; a name neither side knows is the only failure. */
static int cmd_info(const char *name)
{
    int found = 0;

    struct manifest m;
    char **files = NULL;
    int nfiles = 0;
    if (db_read(name, &m, &files, &nfiles) == 0) {
        found = 1;
        printf("Installed:   yes\n");
        printf("Name:        %s\n", m.name[0] ? m.name : name);
        printf("Version:     %s\n", m.version);
        printf("Type:        %s\n", m.type);
        printf("Description: %s\n", m.description);
        printf("Files:       %d\n", nfiles);
        for (int i = 0; i < nfiles; i++) {
            printf("  /%s\n", files[i]);
            free(files[i]);
        }
        free(files);
    } else {
        printf("Installed:   no\n");
    }

    struct repo repos[MAX_REPOS];
    int nrepos = load_repos(repos);
    struct index_entry e;
    struct repo repo;
    if (nrepos > 0 && find_in_repos(name, repos, nrepos, &e, &repo) == 0) {
        found = 1;
        char sizebuf[16];
        human_size(e.size, sizebuf, sizeof(sizebuf));
        printf("Available:   %s %s (%s) from %s\n", e.name, e.version, e.type, repo.url);
        printf("Archive:     %s (%s)\n", e.file, sizebuf);
        if (e.sha256[0]) printf("SHA256:      %s\n", e.sha256);
        if (!is_installed(name)) printf("Summary:     %s\n", e.description);
    } else {
        printf("Available:   no (not in any configured repository)\n");
    }

    if (!found) {
        fprintf(stderr, "pkg: nothing known about '%s'\n", name);
        return 1;
    }
    return 0;
}

/* ── Repository configuration (/etc/pkg/repos.conf) ──────────────────── */
static int cmd_repo_list(void)
{
    struct repo repos[MAX_REPOS];
    int n = load_repos(repos);
    if (n == 0) {
        printf("(no repositories configured — %s is missing or empty)\n", REPOS_CONF);
        return 0;
    }
    for (int i = 0; i < n; i++)
        printf("%d. %-44s [%s]\n", i + 1, repos[i].url, repos[i].is_http ? "http" : "local");
    return 0;
}

static int cmd_repo_add(const char *url)
{
    struct repo probe;
    if (parse_repo_url(url, &probe) != 0) {
        fprintf(stderr, "pkg: '%s' is not a repository URL "
                        "(expected file:///path or http://host[:port]/path)\n", url);
        return 1;
    }

    struct repo repos[MAX_REPOS];
    int n = load_repos(repos);
    for (int i = 0; i < n; i++) {
        if (strcmp(repos[i].url, url) == 0) {
            printf("pkg: %s is already configured\n", url);
            return 0;
        }
    }
    if (n >= MAX_REPOS) {
        fprintf(stderr, "pkg: already at the %d-repository limit\n", MAX_REPOS);
        return 1;
    }

    mkdir_p("/etc/pkg");
    FILE *f = fopen(REPOS_CONF, "a");
    if (!f) {
        fprintf(stderr, "pkg: cannot write %s: %s\n", REPOS_CONF, strerror(errno));
        return 1;
    }
    fprintf(f, "%s\n", url);
    fclose(f);
    printf("pkg: added %s\n", url);
    return 0;
}

/* Rewrites repos.conf without `url`, keeping every other line — comments
 * included — as it found them, so a hand-written config survives being
 * edited by this tool. (Blank lines are the one thing not preserved: the
 * line reader drops them, and a repository list is not whitespace.) */
static int cmd_repo_remove(const char *url)
{
    int n_lines = 0;
    char **lines = read_lines(REPOS_CONF, &n_lines);
    if (!lines) {
        /* No lines can mean either "unreadable" or "there but empty", and
         * only the first is an error worth a strerror(). */
        struct stat st;
        if (stat(REPOS_CONF, &st) != 0) {
            fprintf(stderr, "pkg: cannot read %s: %s\n", REPOS_CONF, strerror(errno));
            return 1;
        }
    }

    int removed = 0;
    FILE *f = fopen(REPOS_CONF, "w");
    if (!f) {
        fprintf(stderr, "pkg: cannot write %s: %s\n", REPOS_CONF, strerror(errno));
        free_lines(lines, n_lines);
        return 1;
    }
    for (int i = 0; i < n_lines; i++) {
        if (lines[i][0] != '#' && strcmp(lines[i], url) == 0) { removed++; continue; }
        fprintf(f, "%s\n", lines[i]);
    }
    fclose(f);
    free_lines(lines, n_lines);

    if (!removed) {
        fprintf(stderr, "pkg: %s is not configured\n", url);
        return 1;
    }
    printf("pkg: removed %s\n", url);
    return 0;
}

static int cmd_repo(int argc, char **argv)
{
    /* argv[1] == "repo" */
    const char *sub = argc > 2 ? argv[2] : "list";
    if (strcmp(sub, "list") == 0) return cmd_repo_list();
    if (strcmp(sub, "add") == 0 && argc > 3) return cmd_repo_add(argv[3]);
    if (strcmp(sub, "remove") == 0 && argc > 3) return cmd_repo_remove(argv[3]);
    fprintf(stderr, "Usage: pkg repo list\n"
                    "       pkg repo add <url>\n"
                    "       pkg repo remove <url>\n");
    return 1;
}

static void usage(void)
{
    fprintf(stderr,
        "Usage: pkg install [--force] <name>...\n"
        "       pkg remove <name>...\n"
        "       pkg list [--available]\n"
        "       pkg search <term>\n"
        "       pkg info <name>\n"
        "       pkg repo list | add <url> | remove <url>\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 1; }

    if (strcmp(argv[1], "install") == 0) {
        int force = 0, names = 0, rc = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0) { force = 1; continue; }
        }
        for (int i = 2; i < argc; i++) {
            if (argv[i][0] == '-') continue;
            names++;
            if (cmd_install(argv[i], force) != 0) rc = 1;
        }
        if (names == 0) { usage(); return 1; }
        return rc;
    }
    if (strcmp(argv[1], "remove") == 0 || strcmp(argv[1], "uninstall") == 0) {
        if (argc < 3) { usage(); return 1; }
        int rc = 0;
        for (int i = 2; i < argc; i++)
            if (cmd_remove(argv[i]) != 0) rc = 1;
        return rc;
    }
    if (strcmp(argv[1], "list") == 0) {
        return cmd_list(argc > 2 && strcmp(argv[2], "--available") == 0);
    }
    if (strcmp(argv[1], "search") == 0) {
        if (argc < 3) { usage(); return 1; }
        return cmd_search(argv[2]);
    }
    if (strcmp(argv[1], "info") == 0) {
        if (argc < 3) { usage(); return 1; }
        return cmd_info(argv[2]);
    }
    if (strcmp(argv[1], "repo") == 0) {
        return cmd_repo(argc, argv);
    }

    usage();
    return 1;
}
