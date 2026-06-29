#!/usr/bin/env bash
# Automated script to build an x86_64-elf cross-compiler toolchain (Binutils + GCC)
set -e

PREFIX="$HOME/opt/cross-x86_64"
TARGET=x86_64-elf
PATH="$PREFIX/bin:$PATH"

BINUTILS_VERSION=2.42
GCC_VERSION=14.2.0

BUILD_DIR="build/toolchain_build"
mkdir -p "$BUILD_DIR"
mkdir -p "$PREFIX"

cd "$BUILD_DIR"

echo "==> Downloading Binutils $BINUTILS_VERSION..."
if [ ! -f "binutils-$BINUTILS_VERSION.tar.xz" ]; then
    curl -LO "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.xz"
fi

echo "==> Downloading GCC $GCC_VERSION..."
if [ ! -f "gcc-$GCC_VERSION.tar.xz" ]; then
    curl -LO "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz"
fi

echo "==> Extracting packages..."
if [ ! -d "binutils-$BINUTILS_VERSION" ]; then
    tar -xf "binutils-$BINUTILS_VERSION.tar.xz"
fi
if [ ! -d "gcc-$GCC_VERSION" ]; then
    tar -xf "gcc-$GCC_VERSION.tar.xz"
    cd "gcc-$GCC_VERSION"
    ./contrib/download_prerequisites
    cd ..
fi

echo "==> Building Binutils..."
mkdir -p build-binutils
cd build-binutils
../binutils-$BINUTILS_VERSION/configure --target=$TARGET --prefix="$PREFIX" --with-sysroot --disable-nls --disable-werror
make -j$(nproc)
make install
cd ..

echo "==> Building GCC..."
mkdir -p build-gcc
cd build-gcc
../gcc-$GCC_VERSION/configure --target=$TARGET --prefix="$PREFIX" --disable-nls --enable-languages=c,c++ --without-headers
make -j$(nproc) all-gcc
make -j$(nproc) all-target-libgcc
make install-gcc
make install-target-libgcc
cd ..

echo "==> Toolchain compiled successfully! Installed to: $PREFIX"
