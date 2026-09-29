#!/usr/bin/env bash
# Build the dedicated x86_64-elf cross-compiler (binutils + GCC, C only) into
# ~/opt/cross-x86_64, where mk/toolchain.mk looks for it first.
#
# You do not need this to build AzamiOS: on an x86_64 host the system gcc works
# (see mk/toolchain.mk for why), and scripts/devenv.sh builds inside a container
# with no host setup at all. A dedicated cross-compiler is still the right tool
# when the host gcc is unusually old or new, when the host is not x86_64, or
# when you want the build to be independent of distribution compiler defaults.
#
#   scripts/build_toolchain.sh              build and install
#   PREFIX=/some/dir scripts/build_toolchain.sh
#   JOBS=8 scripts/build_toolchain.sh
#
# Re-running is cheap: downloads are cached and verified, and a toolchain that
# is already installed at the requested versions is left alone.
set -euo pipefail

PREFIX=${PREFIX:-$HOME/opt/cross-x86_64}
TARGET=x86_64-elf
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}

BINUTILS_VERSION=2.44
GCC_VERSION=14.3.0
# SHA-256 of the release tarballs, so a truncated download or a tampered
# mirror is caught before it is built and installed.
BINUTILS_SHA256=ce2017e059d63e67ddb9240e9d4ec49c2893605035cd60e92ad53177f4377237
GCC_SHA256=e0dc77297625631ac8e50fa92fffefe899a4eb702592da5c32ef04e2293aca3a

GNU_MIRROR=${GNU_MIRROR:-https://ftpmirror.gnu.org/gnu}

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK=${WORK:-$ROOT/build/toolchain}
mkdir -p "$WORK" "$PREFIX"
export PATH="$PREFIX/bin:$PATH"

installed_version() {
    "$PREFIX/bin/$TARGET-$1" --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?$' || true
}
if [ "$(installed_version gcc)" = "$GCC_VERSION" ] &&
   [ "$(installed_version ld)" = "$BINUTILS_VERSION" ]; then
    echo "==> $TARGET-gcc $GCC_VERSION and binutils $BINUTILS_VERSION already in $PREFIX"
    exit 0
fi

for tool in curl tar make gcc g++ bison flex makeinfo; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "error: '$tool' is required to build the cross-compiler." >&2
        echo "       Fedora: sudo dnf install gcc gcc-c++ make bison flex texinfo gmp-devel mpfr-devel libmpc-devel curl" >&2
        echo "       Debian: sudo apt-get install build-essential bison flex texinfo libgmp-dev libmpfr-dev libmpc-dev curl" >&2
        exit 1
    fi
done

fetch() {   # fetch <url> <file> <sha256>
    local url=$1 file=$2 sum=$3
    if [ -f "$WORK/$file" ] && echo "$sum  $WORK/$file" | sha256sum -c --status; then
        return 0
    fi
    echo "==> Downloading $file"
    curl -fL --retry 3 -o "$WORK/$file.part" "$url"
    if ! echo "$sum  $WORK/$file.part" | sha256sum -c --status; then
        echo "error: checksum mismatch for $file" >&2
        rm -f "$WORK/$file.part"
        exit 1
    fi
    mv "$WORK/$file.part" "$WORK/$file"
}

fetch "$GNU_MIRROR/binutils/binutils-$BINUTILS_VERSION.tar.xz" \
      "binutils-$BINUTILS_VERSION.tar.xz" "$BINUTILS_SHA256"
fetch "$GNU_MIRROR/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz" \
      "gcc-$GCC_VERSION.tar.xz" "$GCC_SHA256"

cd "$WORK"
[ -d "binutils-$BINUTILS_VERSION" ] || tar -xf "binutils-$BINUTILS_VERSION.tar.xz"
if [ ! -d "gcc-$GCC_VERSION" ]; then
    tar -xf "gcc-$GCC_VERSION.tar.xz"
    # Build GMP/MPFR/MPC in-tree unless the host provides their headers.
    if ! printf '#include <mpc.h>\n' | gcc -E -x c - >/dev/null 2>&1; then
        (cd "gcc-$GCC_VERSION" && ./contrib/download_prerequisites)
    fi
fi

echo "==> Building binutils $BINUTILS_VERSION"
rm -rf build-binutils && mkdir build-binutils && cd build-binutils
"../binutils-$BINUTILS_VERSION/configure" --target=$TARGET --prefix="$PREFIX" \
    --with-sysroot --disable-nls --disable-werror --disable-gdb --disable-gprofng
make -j"$JOBS"
make install
cd ..

echo "==> Building gcc $GCC_VERSION"
rm -rf build-gcc && mkdir build-gcc && cd build-gcc
# --without-headers: there is no target libc to build against; every AzamiOS
# object is -ffreestanding. libgcc is built with -mno-red-zone so its helpers
# are safe to call from kernel code, which runs without a red zone.
"../gcc-$GCC_VERSION/configure" --target=$TARGET --prefix="$PREFIX" \
    --disable-nls --enable-languages=c --without-headers \
    --disable-multilib --disable-shared --disable-threads
make -j"$JOBS" all-gcc
make -j"$JOBS" all-target-libgcc CFLAGS_FOR_TARGET='-O2 -mcmodel=kernel -mno-red-zone'
make install-gcc install-target-libgcc
cd ..

echo "==> Installed $TARGET toolchain to $PREFIX"
"$PREFIX/bin/$TARGET-gcc" --version | head -1
