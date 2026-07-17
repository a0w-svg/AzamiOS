#!/usr/bin/env bash
# =============================================================================
# port_gdb.sh — Build and install cross-GDB toolchain for AzamiOS
# =============================================================================
#
# WHAT ME DO:
#   1. Download GDB source tarball from GNU FTP
#   2. Extract package and prepare cross-build environment
# USAGE:
#   bash scripts/port_gdb.sh            # builds for x86_64
#
# PREREQS:
#   - Cross-compiler toolchain directory created (run build_toolchain.sh first)
#   - curl, tar, make, gcc available on PATH
# =============================================================================
set -e

GDB_VERSION="15.2"
GDB_URL="https://ftp.gnu.org/gnu/gdb/gdb-${GDB_VERSION}.tar.xz"

ARCH="x86_64"

build_target_gdb() {
    local target_arch="x86_64"
    local TARGET="x86_64-elf"
    local PREFIX=""

    for _p in "$HOME/opt/cross-x86_64" "$HOME/opt/cross" "$HOME/cross"; do
        if [ -d "$_p" ]; then PREFIX="$_p"; break; fi
    done
    PREFIX="${PREFIX:-$HOME/opt/cross-x86_64}"

    echo "================================================================="
    echo "==> ME BUILD GDB FOR: $TARGET"
    echo "==> INSTALL PREFIX : $PREFIX"
    echo "================================================================="

    BUILD_DIR="build/toolchain_build"
    mkdir -p "$BUILD_DIR"
    mkdir -p "$PREFIX"

    cd "$BUILD_DIR"

    echo "==> Downloading GDB $GDB_VERSION..."
    if [ ! -f "gdb-$GDB_VERSION.tar.xz" ]; then
        curl -LO "$GDB_URL"
    fi

    echo "==> Extracting GDB..."
    if [ ! -d "gdb-$GDB_VERSION" ]; then
        tar -xf "gdb-$GDB_VERSION.tar.xz"
    fi

    echo "==> Configuring GDB ($TARGET)..."
    mkdir -p "build-gdb-$TARGET"
    cd "build-gdb-$TARGET"

    # We disable python/guile/nls/werror for robust bare-metal cross-building without host dependency headaches
    ../gdb-$GDB_VERSION/configure \
        --target="$TARGET" \
        --prefix="$PREFIX" \
        --with-sysroot \
        --disable-nls \
        --disable-werror \
        --with-python=no \
        --with-guile=no \
        --enable-tui

    echo "==> Compiling GDB ($TARGET)... This take few minutes!"
    make -j$(nproc)

    echo "==> Installing GDB to $PREFIX/bin..."
    make install

    cd ../..
    echo "==> SUCCESS! GDB installed: $PREFIX/bin/$TARGET-gdb"
    echo ""
}

build_target_gdb "x86_64"

echo "================================================================="
echo "ME DONE! GDB toolchain ready to hunt bugs!"
echo "Run: 'make run-debug' in terminal 1, and 'make gdb' in terminal 2!"
echo "================================================================="
