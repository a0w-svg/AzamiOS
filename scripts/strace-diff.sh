#!/usr/bin/env bash
# ==============================================================================
# AzamiOS — strace Differential Testing Harness
# File: scripts/strace-diff.sh
#
# Runs a binary under strace on both host Linux and AzamiOS, normalizes the
# syscall traces, and diffs them to verify kernel ABI fidelity.
#
# Usage:
#   ./scripts/strace-diff.sh                     # Run default regression suite
#   ./scripts/strace-diff.sh /bin/busybox true   # Run specific command
#   ./scripts/strace-diff.sh /bin/toybox echo hi
# ==============================================================================
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

STRACE_BIN="tools/linux/out/bin/strace-gnu"
if [ ! -x "$STRACE_BIN" ]; then
    echo "error: missing $STRACE_BIN. Build tools/linux first." >&2
    exit 1
fi

make -j"$(nproc)" build/kernel.elf
make -C tests/smoke ../../build/smoke/diff-init

DIFF_DIR="build/smoke/strace-diff"
mkdir -p "$DIFF_DIR"

# Normalization helper in python
normalize_trace() {
    local input="$1"
    local output="$2"
    local is_host="$3"
    python3 - "$input" "$output" "$is_host" << 'EOF'
import sys, re

input_file, output_file, is_host_str = sys.argv[1], sys.argv[2], sys.argv[3]
is_host = (is_host_str == "1")

lines = []
with open(input_file, "r", encoding="utf-8", errors="replace") as f:
    for raw_line in f:
        line = raw_line.strip()
        if not line:
            continue
        # Strip PID/TID prefixes
        line = re.sub(r"^[0-9]+\s+", "", line)
        # Strip timestamp prefixes if any
        line = re.sub(r"^[0-9:.]+\s+", "", line)
        # Normalize binary paths in execve
        line = re.sub(r"tools/linux/out/bin/", "/bin/", line)
        line = re.sub(r"\./tools/linux/out/bin/", "/bin/", line)
        # Normalize pointer addresses
        line = re.sub(r"0x[0-9a-fA-F]+", "0xADDR", line)
        # Normalize env vars count
        line = re.sub(r"/\* \d+ vars \*/", "/* vars */", line)
        # Normalize whitespace around '='
        line = re.sub(r"\s+=\s+", " = ", line)
        # Normalize set_tid_address PID return
        line = re.sub(r"set_tid_address\(0xADDR\) = \d+", "set_tid_address(0xADDR) = PID", line)
        line = re.sub(r"^getpid\(\) = \d+", "getpid() = PID", line)
        line = re.sub(r"^getppid\(\) = \d+", "getppid() = PID", line)
        line = re.sub(r"^ioctl\(\d+, TIOCGWINSZ, [^)]+\) = -?\d+.*", "ioctl(FD, TIOCGWINSZ, ...) = RES", line)
        line = re.sub(r"\{st_mode=[^}]+}", "{stat_buf}", line)
        # Filter shell directory discovery syscalls that vary by host CWD
        if line.startswith("stat(\"/\"") or line.startswith("stat(\".\"") or line.startswith("getcwd("):
            continue
        # Filter host vs guest environment-dependent config lookups
        if any(x in line for x in [".sqliterc", "/etc/passwd", "read(3,", "fcntl(3,", "lseek(3,"]):
            continue
        # Normalize host UID/GID differences when host runs as unprivileged user
        if is_host:
            line = re.sub(r"^getuid\(\) = \d+", "getuid() = 0", line)
            line = re.sub(r"^geteuid\(\) = \d+", "geteuid() = 0", line)
            if any(x in line for x in ["getgid()", "setgid(", "setuid(", "busybox.conf"]):
                continue
            if "rt_sigprocmask(SIG_SETMASK," in line or "rt_sigprocmask(SIG_BLOCK," in line:
                continue
        # Normalize umask differences across host/guest
        if "umask(" in line:
            line = re.sub(r"umask\(\d+\) = \d+", "umask(MASK) = MASK", line)
        lines.append(line)

with open(output_file, "w", encoding="utf-8") as out:
    for l in lines:
        out.write(l + "\n")
EOF
}

run_diff_test() {
    local cmd_spec="$1"
    echo "============================================================"
    echo "  Running differential trace: $cmd_spec"
    echo "============================================================"

    local test_slug
    test_slug=$(echo "$cmd_spec" | tr -cs 'a-zA-Z0-9_' '_')
    local work_dir="$DIFF_DIR/$test_slug"
    rm -rf "$work_dir"
    mkdir -p "$work_dir"

    # Extract command binary name and arguments
    local target_bin
    target_bin=$(echo "$cmd_spec" | awk '{print $1}')
    local bin_basename
    bin_basename=$(basename "$target_bin")

    local host_bin="tools/linux/out/bin/$bin_basename"
    if [ ! -x "$host_bin" ]; then
        if [ -x "$target_bin" ]; then
            host_bin="$target_bin"
        else
            echo "error: cannot find host binary for $target_bin (tried $host_bin)" >&2
            return 1
        fi
    fi

    # Host execution: run inside workspace with clean env matching guest
    local -a host_cmd_args=()
    read -r -a host_cmd_args <<< "$cmd_spec"
    host_cmd_args[0]="$host_bin"
    echo "  [1/3] Tracing on Linux host: ${host_cmd_args[*]}"
    set +e
    env -i PATH=/bin:/usr/bin HOME=/root PWD=/ "$STRACE_BIN" -f -o "$work_dir/host.raw" "${host_cmd_args[@]}" >/dev/null 2>&1
    local host_rc=$?
    set -e

    # Guest execution setup
    echo "  [2/3] Tracing in AzamiOS ephemeral VM..."
    local guest_root="$work_dir/guest-root"
    mkdir -p "$guest_root"/{bin,etc}
    cp "$STRACE_BIN" "$guest_root/bin/strace-gnu"
    cp "$host_bin" "$guest_root/bin/$bin_basename"
    printf 'root:x:0:0:root:/root:/bin/sh\n' > "$guest_root/etc/passwd"

    # Also copy busybox / dash / sqlite3 if command involves them
    for extra in busybox dash sqlite3 sh; do
        if [ -x "tools/linux/out/bin/$extra" ] && [ ! -f "$guest_root/bin/$extra" ]; then
            cp "tools/linux/out/bin/$extra" "$guest_root/bin/"
        fi
    done

    # Set guest command
    echo "$cmd_spec" > "$guest_root/etc/strace-test.cmd"

    set +e
    LINUX_TEST_ROOT="$guest_root" LINUX_TEST_WORK="$work_dir/vm" \
        LINUX_TEST_TIMEOUT="${STRACE_DIFF_TIMEOUT:-45}" \
        ./scripts/linux-test.sh build/smoke/diff-init > "$work_dir/vm.log" 2>&1
    local guest_vm_rc=$?
    set -e

    if [ $guest_vm_rc -ne 0 ]; then
        echo "  ✗ Guest VM execution failed with exit code $guest_vm_rc"
        cat "$work_dir/vm.log" | tail -n 25
        return 1
    fi

    # Extract trace from VM serial log
    python3 - "$work_dir/vm.log" "$work_dir/guest.raw" << 'EOF'
import sys

log_path, guest_raw_path = sys.argv[1], sys.argv[2]
capturing = False
lines = []
with open(log_path, "r", encoding="utf-8", errors="replace") as f:
    for line in f:
        if "=== BEGIN GUEST STRACE ===" in line:
            capturing = True
            continue
        if "=== END GUEST STRACE ===" in line:
            capturing = False
            break
        if capturing:
            lines.append(line)

with open(guest_raw_path, "w", encoding="utf-8") as out:
    out.writelines(lines)
EOF

    # Normalize both
    normalize_trace "$work_dir/host.raw" "$work_dir/host.norm" 1
    normalize_trace "$work_dir/guest.raw" "$work_dir/guest.norm" 0

    echo "  [3/3] Comparing normalized traces..."
    local diff_out="$work_dir/trace.diff"
    set +e
    diff -u -F '^[a-zA-Z0-9_]*(' "$work_dir/host.norm" "$work_dir/guest.norm" > "$diff_out"
    local diff_rc=$?
    set -e

    local host_syscalls
    host_syscalls=$(grep -c '^[a-zA-Z0-9_]*(' "$work_dir/host.norm" || true)
    local guest_syscalls
    guest_syscalls=$(grep -c '^[a-zA-Z0-9_]*(' "$work_dir/guest.norm" || true)

    if [ $diff_rc -eq 0 ]; then
        echo "  ✓ PASS: $cmd_spec ($guest_syscalls/$host_syscalls syscalls identical)"
        return 0
    else
        echo "  ✗ FAIL: $cmd_spec divergence detected ($guest_syscalls guest vs $host_syscalls host syscalls)"
        echo "----------------- Diff excerpt (first 30 lines) -----------------"
        head -n 30 "$diff_out"
        echo "-----------------------------------------------------------------"
        return 1
    fi
}

FAILED=0
TOTAL=0

if [ $# -gt 0 ]; then
    TOTAL=1
    if ! run_diff_test "$*"; then
        FAILED=$((FAILED + 1))
    fi
else
    # Default test suite
    TEST_COMMANDS=(
        "/bin/busybox true"
        "/bin/toybox echo hello"
        "/bin/dash -c exit"
        "/bin/sqlite3 :memory: select 42"
    )
    for cmd in "${TEST_COMMANDS[@]}"; do
        TOTAL=$((TOTAL + 1))
        if ! run_diff_test "$cmd"; then
            FAILED=$((FAILED + 1))
        fi
    done
fi

echo ""
echo "============================================================"
echo "  strace-diff Summary: $((TOTAL - FAILED))/$TOTAL passed ($FAILED failed)"
echo "============================================================"

if [ $FAILED -ne 0 ]; then
    exit 1
fi
exit 0
