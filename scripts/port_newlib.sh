#!/usr/bin/env bash
# =============================================================================
# port_newlib.sh — Build and install newlib for AzamiOS
# =============================================================================
#
# WHAT ME DO:
#   1. Download newlib 4.4.0 source tarball (sourceware.org)
#   2. Provide a minimal libgloss syscall backend that forwards calls through
#      the AzamiOS int $128 interrupt gate (mirrors user/libc/syscalls.c)
#   3. Configure + build newlib with the AzamiOS cross-compiler
#   4. Install headers + libc.a/libm.a into the cross-compiler sysroot so
#      that `make USE_NEWLIB=1` picks them up automatically
#
# USAGE:
# USAGE:
#   bash scripts/port_newlib.sh            # builds for x86_64 (default)
#
# PREREQS:
#   - Cross-compiler already installed (run scripts/build_toolchain.sh first)
#   - curl, tar, make available on PATH
#
# After this script completes, build AzamiOS with:
#   make USE_NEWLIB=1 all
# =============================================================================
set -e

# ── Configuration ─────────────────────────────────────────────────────────────
# Newlib uses dated snapshot filenames: newlib-X.Y.Z.YYYYMMDD.tar.gz
NEWLIB_VERSION=4.4.0.20231231
NEWLIB_URL="https://sourceware.org/pub/newlib/newlib-${NEWLIB_VERSION}.tar.gz"

ARCH="x86_64"
TARGET=x86_64-elf

# Auto-detect prefix
for _p in "$HOME/opt/cross-x86_64" "$HOME/opt/cross" "$HOME/cross"; do
    if [ -f "$_p/bin/${TARGET}-gcc" ]; then PREFIX="$_p"; break; fi
done
PREFIX="${PREFIX:-$HOME/opt/cross-x86_64}"

SYSROOT="$PREFIX/$TARGET"
# Run from repo root regardless of where the script is called from
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/newlib_build"

export PATH="$PREFIX/bin:$PATH"

echo "==> AzamiOS newlib port v${NEWLIB_VERSION}"
echo "    Arch    : $ARCH"
echo "    Target  : $TARGET"
echo "    Prefix  : $PREFIX"
echo "    Sysroot : $SYSROOT"
echo ""

# ── Sanity: cross-compiler must exist ────────────────────────────────────────
if ! command -v "${TARGET}-gcc" >/dev/null 2>&1; then
    echo "ERROR: ${TARGET}-gcc not found in PATH."
    echo "       Run scripts/build_toolchain.sh first, then re-run this script."
    exit 1
fi

# ── Directories ───────────────────────────────────────────────────────────────
mkdir -p "$BUILD_DIR"
mkdir -p "$SYSROOT/lib"
mkdir -p "$SYSROOT/include"
cd "$BUILD_DIR"

# ── Download newlib ───────────────────────────────────────────────────────────
echo "==> Downloading newlib ${NEWLIB_VERSION}..."
if [ ! -f "newlib-${NEWLIB_VERSION}.tar.gz" ]; then
    curl -LO "$NEWLIB_URL"
fi

echo "==> Extracting newlib..."
if [ ! -d "newlib-${NEWLIB_VERSION}" ]; then
    tar -xf "newlib-${NEWLIB_VERSION}.tar.gz"
fi

# ── Write AzamiOS libgloss syscall backend ────────────────────────────────────
# Provides _read/_write/_open/_close/_sbrk/_exit etc. that newlib calls
# internally.  We mirror user/libc/syscalls.c with proper newlib errno
# convention (return -1 and set errno on failure).
GLOSS_DIR="newlib-${NEWLIB_VERSION}/libgloss/azamios"
mkdir -p "$GLOSS_DIR"

echo "==> Writing AzamiOS libgloss syscall backend..."

cat > "$GLOSS_DIR/syscalls.c" << 'SYSCALLS_EOF'
/*
 * azamios/syscalls.c — Newlib libgloss backend for AzamiOS
 * Forwards standard C library I/O and memory requests to the
 * AzamiOS kernel via int $128 (syscall gate, mirrors the
 * ABI in kernel/syscall/syscall.c).
 *
 * Syscall ABI:
 *   EAX/RAX = syscall number
 *   EBX/RBX = arg1, ECX/RCX = arg2, EDX/RDX = arg3
 *   Return value in EAX/RAX (negative on error)
 */
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/times.h>
#include <errno.h>

/* AzamiOS syscall numbers — must match kernel/syscall/include/syscall.h */
#define SYS_EXIT    2
#define SYS_GETPID  24
#define SYS_READ    19
#define SYS_WRITE   20
#define SYS_OPEN    21
#define SYS_CLOSE   22
#define SYS_SBRK    23

#if defined(__x86_64__)
#define _SC3(nr,a,b,c,r) \
    __asm__ volatile("int $128":"=a"(r):"a"((long)(nr)),"b"((long)(a)),"c"((long)(b)),"d"((long)(c)):"memory")
#define _SC1(nr,a,r) \
    __asm__ volatile("int $128":"=a"(r):"a"((long)(nr)),"b"((long)(a)):"memory")
#define _SC0(nr,r) \
    __asm__ volatile("int $128":"=a"(r):"a"((long)(nr)):"memory")
#else
#define _SC3(nr,a,b,c,r) \
    __asm__ volatile("int $128":"=a"(r):"a"((int)(nr)),"b"((int)(a)),"c"((int)(b)),"d"((int)(c)):"memory")
#define _SC1(nr,a,r) \
    __asm__ volatile("int $128":"=a"(r):"a"((int)(nr)),"b"((int)(a)):"memory")
#define _SC0(nr,r) \
    __asm__ volatile("int $128":"=a"(r):"a"((int)(nr)):"memory")
#endif

int _read(int fd, char *buf, int len) {
    int r; _SC3(SYS_READ, fd, buf, len, r);
    if (r < 0) { errno = -r; return -1; }
    return r;
}
int _write(int fd, char *buf, int len) {
    int r; _SC3(SYS_WRITE, fd, buf, len, r);
    if (r < 0) { errno = -r; return -1; }
    return r;
}
int _open(const char *name, int flags, int mode) {
    int r; _SC3(SYS_OPEN, name, flags, mode, r);
    if (r < 0) { errno = -r; return -1; }
    return r;
}
int _close(int fd) {
    int r; _SC1(SYS_CLOSE, fd, r);
    if (r < 0) { errno = -r; return -1; }
    return r;
}
void *_sbrk(int incr) {
    extern char __heap_start;
    static char *heap_end = (char *)0;
    char *prev;
    if (heap_end == 0) heap_end = &__heap_start;
    prev = heap_end;
    int r; _SC1(SYS_SBRK, incr, r);
    if (r < 0) { errno = 12 /* ENOMEM */; return (void *)-1; }
    heap_end += incr;
    return (void *)prev;
}
int _fstat(int fd, struct stat *st) {
    (void)fd; st->st_mode = S_IFCHR; return 0;
}
int _isatty(int fd)                   { return (fd <= 2) ? 1 : 0; }
int _lseek(int fd, int off, int w)    { (void)fd;(void)off;(void)w; return 0; }
int _getpid(void) {
    int r; _SC0(SYS_GETPID, r); return r;
}
int _kill(int pid, int sig) {
    (void)pid;(void)sig; errno = 22; return -1;
}
void _exit(int status) {
    __asm__ volatile("int $128"::"a"(SYS_EXIT),"b"(status):"memory");
    while(1);
}
clock_t _times(struct tms *buf) { (void)buf; return (clock_t)-1; }

/* AzamiOS-specific: exec() — non-POSIX, used by the WM terminal */
void exec(const char *filename) {
    __asm__ volatile("int $128"::"a"(10),"b"(filename):"memory");
}
SYSCALLS_EOF

# Minimal Makefile for the libgloss azamios target ────────────────────────────
cat > "$GLOSS_DIR/Makefile.in" << 'MKEOF'
# Makefile.in — azamios libgloss
DESTDIR   =
VPATH     = @srcdir@
srcdir    = @srcdir@
tooldir   = $(exec_prefix)/$(target_alias)
CC        = @CC@
AR        = @AR@
RANLIB    = @RANLIB@
CFLAGS    = @CFLAGS@
INCLUDES  = -I$(srcdir)/.. -I$(srcdir)/../../newlib/libc/include
LIB_OBJS  = syscalls.o

all: libnosys.a

libnosys.a: $(LIB_OBJS)
$(AR) qv $@ $?
$(RANLIB) $@

%.o: $(srcdir)/%.c
$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

install: all
mkdir -p $(DESTDIR)$(tooldir)/lib
$(INSTALL_DATA) libnosys.a $(DESTDIR)$(tooldir)/lib/libnosys.a

clean mostlyclean distclean maintainer-clean:
rm -f *.o *.a
MKEOF

# Minimal configure script (autoconf-free stub) ───────────────────────────────
cat > "$GLOSS_DIR/configure" << 'CFGEOF'
#!/bin/sh
srcdir="$(dirname "$0")"
sed \
  -e "s|@srcdir@|$srcdir|g"     \
  -e "s|@CC@|${CC:-gcc}|g"      \
  -e "s|@AR@|${AR:-ar}|g"       \
  -e "s|@RANLIB@|${RANLIB:-ranlib}|g" \
  -e "s|@CFLAGS@|${CFLAGS}|g"  \
  "$srcdir/Makefile.in" > Makefile
CFGEOF
chmod +x "$GLOSS_DIR/configure"

# ── Configure newlib ──────────────────────────────────────────────────────────
echo "==> Configuring newlib ${NEWLIB_VERSION} for ${TARGET}..."
mkdir -p "build-newlib-${TARGET}"
cd "build-newlib-${TARGET}"

"../newlib-${NEWLIB_VERSION}/configure" \
    --target="$TARGET"                         \
    --prefix="$PREFIX"                         \
    --disable-multilib                         \
    --disable-newlib-supplied-syscalls         \
    --enable-newlib-reent-small                \
    --enable-newlib-io-c99-formats             \
    --enable-newlib-io-long-long               \
    --disable-newlib-multithread               \
    --disable-newlib-iconv                     \
    CC_FOR_TARGET="${TARGET}-gcc"              \
    AS_FOR_TARGET="${TARGET}-as"               \
    LD_FOR_TARGET="${TARGET}-ld"               \
    AR_FOR_TARGET="${TARGET}-ar"               \
    RANLIB_FOR_TARGET="${TARGET}-ranlib"       \
    CFLAGS_FOR_TARGET="-ffreestanding -fno-stack-protector -O2 -g"

# Build ONLY the newlib C library — not libgloss.
# libgloss/i386 contains old K&R C (cygmon-gmon.c) that fails under GCC 14+.
# We provide our own syscall stubs (libnosys.a) instead.
echo "==> Building newlib libc + libm only (skipping libgloss)..."
make -j"$(nproc)" all-target-newlib

echo "==> Installing newlib headers + libs to $PREFIX..."
make install-target-newlib

cd ..


# ── Build and install the AzamiOS syscall stubs (libnosys.a) ─────────────────
# Compile our hand-written int $128 stubs and package them as libnosys.a.
# This replaces the libgloss we skipped above.
echo "==> Building AzamiOS syscall stubs (libnosys.a)..."
mkdir -p "build-gloss-${TARGET}"
cd "build-gloss-${TARGET}"

"${TARGET}-gcc" \
    -ffreestanding -fno-stack-protector -O2 \
    -I"../newlib-${NEWLIB_VERSION}/newlib/libc/include" \
    -c "../newlib-${NEWLIB_VERSION}/libgloss/azamios/syscalls.c" \
    -o syscalls.o

"${TARGET}-ar" rcs libnosys.a syscalls.o
"${TARGET}-ranlib" libnosys.a
install -m 644 libnosys.a "$SYSROOT/lib/libnosys.a"

cd ..


# ── Done ──────────────────────────────────────────────────────────────────────
echo ""
echo "================================================================="
echo " Newlib ${NEWLIB_VERSION} for AzamiOS (${TARGET}) installed!"
echo ""
echo " Headers  : ${SYSROOT}/include"
echo " libc.a   : ${SYSROOT}/lib/libc.a"
echo " libm.a   : ${SYSROOT}/lib/libm.a"
echo " libnosys : ${SYSROOT}/lib/libnosys.a"
echo ""
echo " Build AzamiOS with newlib:"
if [ "$ARCH" = "x86_64" ]; then
echo "   make USE_NEWLIB=1 ARCH=x86_64 all"
else
echo "   make USE_NEWLIB=1 all"
fi
echo "================================================================="
