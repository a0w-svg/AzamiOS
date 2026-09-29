#!/usr/bin/env bash
# `make doctor` — check that this machine can build and run AzamiOS, and say
# exactly what to install when it cannot.
#
# Exit status is 0 when everything *required* for `make && make run` is
# present, 1 otherwise. Optional tools are reported but never fail the check.
#
# Environment (set by the Makefile): CROSS_PREFIX, TOOLCHAIN_SOURCE.
set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
CROSS_PREFIX=${CROSS_PREFIX-}
TOOLCHAIN_SOURCE=${TOOLCHAIN_SOURCE:-unknown}

if [ -t 1 ]; then
    OK=$'\e[32m✓\e[0m'; BAD=$'\e[31m✗\e[0m'; OPT=$'\e[33m•\e[0m'
else
    OK='ok '; BAD='MISSING'; OPT='opt'
fi

missing_required=()
missing_packages=()

# Package names per package manager, keyed by the command we probe for.
pkg_for() {
    local cmd=$1 pm=$2
    case "$pm:$cmd" in
        dnf:gcc|apt:gcc|pacman:gcc|zypper:gcc) echo gcc ;;
        dnf:ld|zypper:ld|pacman:ld) echo binutils ;;
        apt:ld) echo binutils ;;
        *:nasm) echo nasm ;;
        *:make) echo make ;;
        *:python3) [ "$pm" = pacman ] && echo python || echo python3 ;;
        *:mke2fs) echo e2fsprogs ;;
        *:xorriso) echo xorriso ;;
        dnf:qemu-system-x86_64) echo qemu-system-x86-core ;;
        apt:qemu-system-x86_64) echo qemu-system-x86 ;;
        pacman:qemu-system-x86_64) echo qemu-system-x86 ;;
        zypper:qemu-system-x86_64) echo qemu-x86 ;;
        *:ccache) echo ccache ;;
        *:git) echo git ;;
        *:curl) echo curl ;;
        *:gdb) echo gdb ;;
        *) echo "$cmd" ;;
    esac
}

PM=
for p in dnf apt-get pacman zypper; do
    command -v "$p" >/dev/null 2>&1 && { PM=${p%-get}; break; }
done

check() {   # check <required|optional> <command> <what it is for>
    local need=$1 cmd=$2 why=$3 path
    if path=$(command -v "$cmd" 2>/dev/null); then
        printf '  %s %-22s %s\n' "$OK" "$cmd" "$path"
        return 0
    fi
    if [ "$need" = required ]; then
        printf '  %s %-22s needed for %s\n' "$BAD" "$cmd" "$why"
        missing_required+=("$cmd")
    else
        printf '  %s %-22s optional: %s\n' "$OPT" "$cmd" "$why"
    fi
    [ -n "$PM" ] && missing_packages+=("$(pkg_for "$cmd" "$PM")")
    return 1
}

echo "AzamiOS build environment"
echo
echo "Compiler"
cc="${CROSS_PREFIX}gcc"
if command -v "$cc" >/dev/null 2>&1; then
    printf '  %s %-22s %s\n' "$OK" "$cc" "$("$cc" --version | head -1)"
    printf '      selected because: %s\n' "$TOOLCHAIN_SOURCE"
    target=$("$cc" -dumpmachine 2>/dev/null)
    case "$target" in
        x86_64-*) ;;
        *) printf '  %s compiler targets %s, not x86_64\n' "$BAD" "$target"
           missing_required+=("x86_64 compiler") ;;
    esac
else
    printf '  %s %-22s no x86_64 C compiler (%s)\n' "$BAD" "$cc" "$TOOLCHAIN_SOURCE"
    missing_required+=(gcc)
    [ -n "$PM" ] && missing_packages+=("$(pkg_for gcc "$PM")")
fi
if command -v "${CROSS_PREFIX}ld" >/dev/null 2>&1; then
    printf '  %s %-22s %s\n' "$OK" "${CROSS_PREFIX}ld" "$("${CROSS_PREFIX}ld" --version | head -1)"
else
    printf '  %s %-22s linker\n' "$BAD" "${CROSS_PREFIX}ld"
    missing_required+=(ld)
    [ -n "$PM" ] && missing_packages+=("$(pkg_for ld "$PM")")
fi

echo
echo "Build tools"
check required nasm    "assembling arch/x86_64/*.asm"
check required make    "the build itself"
check required python3 "disk image and asset generators in scripts/"
check required mke2fs  "the ext2 root and boot partitions"
check optional ccache  "much faster rebuilds (auto-detected)"
check optional xorriso "\`make iso\`"
check optional git     "bootstrapping the Limine bootloader into tools/limine"
check optional curl    "\`make linux\` / ports (fetches upstream sources)"

echo
echo "Running"
check required qemu-system-x86_64 "\`make run\`"
check optional gdb "\`make gdb\`"
if [ -e /dev/kvm ]; then
    if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
        printf '  %s %-22s hardware acceleration available\n' "$OK" "/dev/kvm"
    else
        printf '  %s %-22s present but not accessible: sudo usermod -aG kvm %s, then log in again\n' \
            "$OPT" "/dev/kvm" "$(id -un)"
    fi
else
    printf '  %s %-22s absent: QEMU will use (much slower) TCG emulation\n' "$OPT" "/dev/kvm"
fi

if [ -f "$ROOT/tools/limine/limine-bios.sys" ] || [ -f /usr/share/limine/limine-bios.sys ]; then
    printf '  %s %-22s bootloader binaries present\n' "$OK" "limine"
else
    printf '  %s %-22s fetched automatically on first `make` (needs git + network)\n' "$OPT" "limine"
fi

echo
if [ ${#missing_required[@]} -eq 0 ]; then
    echo "Everything needed for \`make && make run\` is installed."
    exit 0
fi

echo "Missing: ${missing_required[*]}"
if [ -n "$PM" ] && [ ${#missing_packages[@]} -gt 0 ]; then
    # De-duplicate while keeping order.
    pkgs=$(printf '%s\n' "${missing_packages[@]}" | awk '!seen[$0]++' | tr '\n' ' ')
    case "$PM" in
        dnf)    echo "Install:  sudo dnf install $pkgs" ;;
        apt)    echo "Install:  sudo apt-get install $pkgs" ;;
        pacman) echo "Install:  sudo pacman -S --needed $pkgs" ;;
        zypper) echo "Install:  sudo zypper install $pkgs" ;;
    esac
fi
if command -v podman >/dev/null 2>&1 || command -v docker >/dev/null 2>&1; then
    echo "Or build without installing anything:  scripts/devenv.sh make run"
fi
exit 1
