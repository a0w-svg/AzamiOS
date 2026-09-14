/* ============================================================================
 * AzamiOS Userspace — Package Manager (pkg.elf)
 * File: userland/apps/pkg/main.c
 *
 * `pkg install <name>` / `pkg remove <name>` / `pkg list [--available]`.
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
 * "name version type file description..." line per package, whitespace-
 * separated for the first four fields, description running to end of
 * line) and the package archives it names. /etc/pkg/repos.conf lists one
 * repository URL per line, checked in order until a package name matches.
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

/* Ensures a single path component's parent exists (mkdir is not recursive
 * here, matching tar.elf — every package this tool ships targets
 * directories the base image already has, bin/sbin/usr/bin/usr/sbin, so a
 * full mkdir -p is more machinery than v1 needs). */
static void ensure_parent_dir(const char *path)
{
    char buf[300];
    snprintf(buf, sizeof(buf), "%s", path);
    char *slash = strrchr(buf, '/');
    if (!slash || slash == buf) return;
    *slash = '\0';
    mkdir(buf, 0755);
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
            mkdir(h.name, 0755);
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
};

static int parse_repo_url(const char *url, struct repo *r)
{
    memset(r, 0, sizeof(*r));
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
        snprintf(r->path, sizeof(r->path), "%s", slash ? slash : "/");
        size_t l = strlen(r->path);
        while (l > 1 && r->path[l - 1] == '/') { r->path[--l] = '\0'; }
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

    /* KNOWN ISSUE: a `pkg install` needs two HTTP fetches — the repo's
     * index, then the package archive — and in testing, only the *first*
     * TCP connection a boot ever makes reliably completes; a second one
     * (same process, a forked child, doesn't matter) blocks in the kernel
     * with no SYN ever reaching the wire (confirmed via a packet capture:
     * zero further traffic after the first connection's close). This
     * looks like a real, pre-existing bug somewhere below tcp_connect() —
     * not something fixed here — so `pkg install`/`pkg list --available`
     * against an http:// repo is exercised and correct in isolation (one
     * fetch, one boot) but not yet reliable back-to-back. file:// repos
     * are unaffected (no sockets involved) and are what the bundled
     * sample repository uses by default; see docs/PACKAGES.md. */
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

/* One index.txt line: "name version type file description...". The first
 * four fields are whitespace-separated; the description is everything
 * after the fourth field, taken verbatim (so it may itself contain
 * spaces). */
struct index_entry {
    char name[64], version[32], type[16], file[128], description[192];
};

static int parse_index_line(const char *line, struct index_entry *e)
{
    memset(e, 0, sizeof(*e));
    int n = sscanf(line, "%63s %31s %15s %127s", e->name, e->version, e->type, e->file);
    if (n < 4) return -1;
    /* Description: skip past the four fields already consumed. */
    const char *p = line;
    for (int i = 0; i < 4 && p; i++) {
        while (*p == ' ' || *p == '\t') p++;
        p = strpbrk(p, " \t");
    }
    if (p) { while (*p == ' ' || *p == '\t') p++; snprintf(e->description, sizeof(e->description), "%s", p); }
    return 0;
}

/* Searches every configured repo's index for `name`; on a match, fills
 * `entry` and `*found_repo` and returns 0. */
static int find_in_repos(const char *name, struct repo repos[MAX_REPOS], int nrepos,
                          struct index_entry *entry, struct repo *found_repo)
{
    for (int i = 0; i < nrepos; i++) {
        char idx_path[] = "/tmp/pkg-index-XXXXXX";
        int fd = mkstemp(idx_path);
        if (fd < 0) continue;
        close(fd);
        if (repo_fetch(&repos[i], "index.txt", idx_path) != 0) { unlink(idx_path); continue; }

        FILE *f = fopen(idx_path, "r");
        if (f) {
            char line[MAX_LINE];
            while (fgets(line, sizeof(line), f)) {
                size_t l = strlen(line);
                while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
                if (l == 0 || line[0] == '#') continue;
                struct index_entry e;
                if (parse_index_line(line, &e) == 0 && strcmp(e.name, name) == 0) {
                    *entry = e;
                    *found_repo = repos[i];
                    fclose(f);
                    unlink(idx_path);
                    return 0;
                }
            }
            fclose(f);
        }
        unlink(idx_path);
    }
    return -1;
}

/* ── Installed-package database (/var/pkg/db/<name>) ────────────────── */
static int db_write(const struct manifest *m, const char *files_tmp_path)
{
    mkdir("/var", 0755);
    mkdir("/var/pkg", 0755);
    mkdir(DB_DIR, 0755);
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
static int cmd_install(const char *name)
{
    char dbpath[256];
    snprintf(dbpath, sizeof(dbpath), "%s/%s", DB_DIR, name);
    struct stat st;
    if (stat(dbpath, &st) == 0) {
        printf("pkg: '%s' is already installed\n", name);
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

    mkdir("/var", 0755); mkdir("/var/pkg", 0755); mkdir(CACHE_DIR, 0755);
    char pkg_path[300];
    snprintf(pkg_path, sizeof(pkg_path), "%s/%s", CACHE_DIR, entry.file);
    printf("pkg: fetching %s (%s %s)...\n", entry.file, entry.name, entry.version);
    if (repo_fetch(&repo, entry.file, pkg_path) != 0) {
        fprintf(stderr, "pkg: failed to fetch %s\n", entry.file);
        return 1;
    }

    char files_tmp[] = "/tmp/pkg-files-XXXXXX";
    int ffd = mkstemp(files_tmp);
    if (ffd < 0) { fprintf(stderr, "pkg: mkstemp failed: %s\n", strerror(errno)); return 1; }
    FILE *files_out = fdopen(ffd, "w");

    struct manifest m;
    printf("pkg: installing %s %s...\n", entry.name, entry.version);
    int rc = extract_package(pkg_path, &m, files_out);
    if (files_out) fclose(files_out);

    if (rc == 0) {
        if (!m.name[0]) snprintf(m.name, sizeof(m.name), "%s", entry.name);
        db_write(&m, files_tmp);
        printf("pkg: installed %s %s — %s\n", m.name, m.version, m.description);
    } else {
        fprintf(stderr, "pkg: install of '%s' failed\n", name);
    }
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

    for (int i = 0; i < nrepos; i++) {
        char idx_path[] = "/tmp/pkg-index-XXXXXX";
        int fd = mkstemp(idx_path);
        if (fd < 0) continue;
        close(fd);
        if (repo_fetch(&repos[i], "index.txt", idx_path) != 0) { unlink(idx_path); continue; }
        FILE *f = fopen(idx_path, "r");
        if (f) {
            char line[MAX_LINE];
            while (fgets(line, sizeof(line), f)) {
                size_t l = strlen(line);
                while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
                if (l == 0 || line[0] == '#') continue;
                struct index_entry e;
                if (parse_index_line(line, &e) == 0)
                    printf("%-16s %-10s %-8s %s\n", e.name, e.version, e.type, e.description);
            }
            fclose(f);
        }
        unlink(idx_path);
    }
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "Usage: pkg install <name>\n"
        "       pkg remove <name>\n"
        "       pkg list [--available]\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 1; }

    if (strcmp(argv[1], "install") == 0) {
        if (argc < 3) { usage(); return 1; }
        return cmd_install(argv[2]);
    }
    if (strcmp(argv[1], "remove") == 0 || strcmp(argv[1], "uninstall") == 0) {
        if (argc < 3) { usage(); return 1; }
        return cmd_remove(argv[2]);
    }
    if (strcmp(argv[1], "list") == 0) {
        return cmd_list(argc > 2 && strcmp(argv[2], "--available") == 0);
    }

    usage();
    return 1;
}
