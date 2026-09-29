#!/usr/bin/env bash
# ==============================================================================
# AzamiOS — complete build
#
#   ./build.sh                  build everything: stock-Linux userland, kernel,
#                               native userland, disk image (hdd.img) and ISO
#   ./build.sh --run            ... then boot it in QEMU
#   ./build.sh --clean --test   rebuild from scratch, then run the ABI probe
#   ./build.sh --help           all options
#
# Works on any machine with either the build tools (`make doctor` lists them)
# or podman/docker: when the host cannot build AzamiOS itself, the script
# re-runs itself inside the build container (scripts/devenv.sh) — nothing to
# install first. Each stage is timed, the full output is kept in
# build/logs/build.log, and the run ends with a summary of what was produced.
#
# The stages are the same ones `make world` runs; this adds environment
# selection, cleaning, timing, logging and the summary around them.
# ==============================================================================
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cd "$ROOT"

usage() {
    cat <<'EOF'
Usage: ./build.sh [options]

Stages
  --no-linux        skip the stock-Linux userland (musl, BusyBox, ABI probe);
                    use this offline, it downloads upstream sources
  --ports           also build the ported software (bash, coreutils, vim,
                    gcc, ... — see tools/linux/ports.mk; takes a while)
  --no-iso          skip the ISO
  --test            boot the Linux-ABI conformance probe after building

Build flavour
  --clean           remove kernel, userland, disk and ISO outputs first
  --distclean       --clean, plus the stock-Linux build output (downloads kept)
  --debug           -O0 build for debugging (DEBUG=1)
  --lto             link-time optimised kernel (LTO=1)
  -j, --jobs N      parallel jobs (default: all CPUs)
  --cmdline "ARGS"  extra kernel parameters for the disk image's boot entries

Afterwards
  --run             boot hdd.img in QEMU (BIOS)
  --run-uefi        boot the ISO in QEMU under OVMF (UEFI)

Environment
  --host            build on this machine, never in the container
  --container       always build in the container (scripts/devenv.sh)
  -q, --quiet       only stage headers on the terminal (everything still logged)
  -h, --help        this text
EOF
}

# ── Options ───────────────────────────────────────────────────────────────────
NO_LINUX=0; PORTS=0; NO_ISO=0; TEST=0
CLEAN=0; DISTCLEAN=0; DEBUG=0; LTO=0; JOBS=""; CMDLINE=""
RUN=""; ENV_MODE=auto; QUIET=0
ORIG_ARGS=("$@")

while [ $# -gt 0 ]; do
    case "$1" in
        --no-linux)   NO_LINUX=1 ;;
        --ports)      PORTS=1 ;;
        --no-iso)     NO_ISO=1 ;;
        --test)       TEST=1 ;;
        --clean)      CLEAN=1 ;;
        --distclean)  CLEAN=1; DISTCLEAN=1 ;;
        --debug)      DEBUG=1 ;;
        --lto)        LTO=1 ;;
        -j|--jobs)    [ $# -ge 2 ] || { echo "build.sh: $1 needs a number" >&2; exit 2; }
                      JOBS=$2; shift ;;
        -j*)          JOBS=${1#-j} ;;
        --cmdline)    [ $# -ge 2 ] || { echo "build.sh: --cmdline needs a value" >&2; exit 2; }
                      CMDLINE=$2; shift ;;
        --run)        RUN=run ;;
        --run-uefi)   RUN=run-uefi; NO_ISO=0 ;;
        --host)       ENV_MODE=host ;;
        --container)  ENV_MODE=container ;;
        -q|--quiet)   QUIET=1 ;;
        -h|--help)    usage; exit 0 ;;
        *)            echo "build.sh: unknown option '$1' (see --help)" >&2; exit 2 ;;
    esac
    shift
done

if [ -n "$JOBS" ] && ! [[ "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
    echo "build.sh: --jobs wants a positive number, got '$JOBS'" >&2
    exit 2
fi

# ── Where to build ────────────────────────────────────────────────────────────
# Inside the container AZAMI_DEVENV is set, so the re-exec below never loops.
#
# `make doctor` covers what `make && make run` needs; the optional stages need
# more, and a host that has the core tools but not those should still build
# in the container rather than fail halfway through.
MISSING_FOR_STAGES=()
stage_tools_present() {
    MISSING_FOR_STAGES=()
    local need=()
    if [ "$NO_ISO" = 0 ] || [ "$TEST" = 1 ] || [ "$RUN" = run-uefi ]; then
        command -v xorriso >/dev/null 2>&1 || command -v mkisofs >/dev/null 2>&1 ||
            command -v genisoimage >/dev/null 2>&1 || MISSING_FOR_STAGES+=(xorriso)
    fi
    [ "$NO_LINUX" = 0 ] && need+=(curl tar gzip bzip2 perl)
    [ "$PORTS" = 1 ]    && need+=(g++ bison flex makeinfo autoconf automake xz patch)
    [ "$TEST" = 1 ] || [ -n "$RUN" ] && need+=(qemu-system-x86_64)
    local t
    for t in "${need[@]}"; do
        command -v "$t" >/dev/null 2>&1 || MISSING_FOR_STAGES+=("$t")
    done
    [ ${#MISSING_FOR_STAGES[@]} -eq 0 ]
}

host_can_build() {
    command -v make >/dev/null 2>&1 || return 1
    # doctor needs make's view of the toolchain (CROSS_PREFIX), so ask make.
    make --no-print-directory doctor >/dev/null 2>&1 || return 1
    stage_tools_present
}

if [ -z "${AZAMI_DEVENV:-}" ]; then
    case "$ENV_MODE" in
        container) use_container=1 ;;
        host)      use_container=0 ;;
        auto)      if host_can_build; then use_container=0; else use_container=1; fi ;;
    esac
    if [ "$use_container" = 1 ]; then
        if ! command -v podman >/dev/null 2>&1 && ! command -v docker >/dev/null 2>&1; then
            echo "build.sh: this machine cannot build AzamiOS, and has no podman or docker" >&2
            echo "          to build in a container instead. What is missing:" >&2
            if command -v make >/dev/null 2>&1; then make --no-print-directory doctor >&2 || true
            else bash scripts/doctor.sh >&2 || true; fi
            exit 1
        fi
        if [ "$ENV_MODE" = auto ]; then
            if [ ${#MISSING_FOR_STAGES[@]} -gt 0 ]; then
                echo "  ▸  Host lacks ${MISSING_FOR_STAGES[*]}; building in the container (scripts/devenv.sh)"
            else
                echo "  ▸  Host lacks build tools; building in the container (scripts/devenv.sh)"
            fi
        fi
        exec scripts/devenv.sh env AZAMI_DEVENV=1 ./build.sh "${ORIG_ARGS[@]}"
    fi
    if [ "$ENV_MODE" = host ] && ! host_can_build; then
        echo "build.sh: --host given, but this machine is missing build tools${MISSING_FOR_STAGES[*]:+: ${MISSING_FOR_STAGES[*]}}" >&2
        if command -v make >/dev/null 2>&1; then make --no-print-directory doctor >&2 || true
        else bash scripts/doctor.sh >&2 || true; fi
        exit 1
    fi
fi

# ── Logging and stages ────────────────────────────────────────────────────────
LOG_DIR=$ROOT/build/logs
mkdir -p "$LOG_DIR"
LOG=$LOG_DIR/build.log
: > "$LOG"

if [ -t 1 ]; then B=$'\e[1m'; G=$'\e[32m'; R=$'\e[31m'; Y=$'\e[33m'; N=$'\e[0m'
else B=""; G=""; R=""; Y=""; N=""; fi

MAKE_ARGS=(--no-print-directory)
[ -n "$JOBS" ]   && MAKE_ARGS+=("-j$JOBS")
[ "$DEBUG" = 1 ] && MAKE_ARGS+=(DEBUG=1)
[ "$LTO" = 1 ]   && MAKE_ARGS+=(LTO=1)
MAKE_ARGS+=("CMDLINE=$CMDLINE")

declare -a STAGE_NAMES=() STAGE_TIMES=() STAGE_RESULTS=()
TOTAL_START=$SECONDS

fmt_time() { local s=$1; printf '%dm%02ds' $((s / 60)) $((s % 60)); }

# stage <name> <command...> — run, log, time; stop the build on failure
# unless the stage is marked optional with STAGE_OPTIONAL=1.
stage() {
    local name=$1; shift
    local start=$SECONDS rc=0
    printf '%s▸ %s%s\n' "$B" "$name" "$N"
    printf '\n===== %s: %s =====\n' "$name" "$*" >> "$LOG"
    if [ "$QUIET" = 1 ]; then
        "$@" >> "$LOG" 2>&1 || rc=$?
    else
        "$@" 2>&1 | tee -a "$LOG" || rc=${PIPESTATUS[0]}
    fi
    local took=$((SECONDS - start))
    STAGE_NAMES+=("$name"); STAGE_TIMES+=("$took")
    if [ "$rc" -eq 0 ]; then
        STAGE_RESULTS+=(ok)
        printf '  %s✓%s %s (%s)\n' "$G" "$N" "$name" "$(fmt_time "$took")"
    elif [ "${STAGE_OPTIONAL:-0}" = 1 ]; then
        STAGE_RESULTS+=(warn)
        printf '  %s⚠%s %s failed (exit %d) — continuing; see %s\n' "$Y" "$N" "$name" "$rc" "$LOG"
    else
        STAGE_RESULTS+=(FAIL)
        printf '  %s✗%s %s failed (exit %d); the last lines of %s:\n' "$R" "$N" "$name" "$rc" "$LOG"
        tail -n 25 "$LOG" | sed 's/^/      /'
        summary
        exit "$rc"
    fi
}

human_size() {
    local f=$1
    [ -e "$f" ] || { echo "—"; return; }
    local b; b=$(stat -c %s "$f")
    if   [ "$b" -ge 1073741824 ]; then awk -v b="$b" 'BEGIN{printf "%.1f GiB", b/1073741824}'
    elif [ "$b" -ge 1048576 ];    then awk -v b="$b" 'BEGIN{printf "%.1f MiB", b/1048576}'
    else awk -v b="$b" 'BEGIN{printf "%.1f KiB", b/1024}'; fi
}

summary() {
    echo
    printf '%sSummary%s  (total %s, log: %s)\n' "$B" "$N" "$(fmt_time $((SECONDS - TOTAL_START)))" "${LOG#"$ROOT"/}"
    local i
    for i in "${!STAGE_NAMES[@]}"; do
        local mark="$G✓$N"
        [ "${STAGE_RESULTS[$i]}" = warn ] && mark="$Y⚠$N"
        [ "${STAGE_RESULTS[$i]}" = FAIL ] && mark="$R✗$N"
        printf '  %s %-46s %s\n' "$mark" "${STAGE_NAMES[$i]}" "$(fmt_time "${STAGE_TIMES[$i]}")"
    done
    echo
    printf '  %-32s %s\n' "build/kernel.elf"      "$(human_size build/kernel.elf)"
    printf '  %-32s %s\n' "hdd.img (BIOS disk)"   "$(human_size hdd.img)"
    [ "$NO_ISO" = 1 ] || printf '  %-32s %s\n' "build/AzamiOS.iso (BIOS+UEFI)" "$(human_size build/AzamiOS.iso)"
    if [ -d userland/build/bin ]; then
        printf '  %-32s %s\n' "native programs" "$(find userland/build/bin userland/build/sbin -name '*.elf' 2>/dev/null | wc -l)"
    fi
    if [ -x tools/linux/out/bin/busybox ]; then
        printf '  %-32s %s applets\n' "BusyBox" "$(tools/linux/out/bin/busybox --list 2>/dev/null | wc -l)"
    fi
    if [ -d tools/linux/out/bin ]; then
        printf '  %-32s %s\n' "stock Linux binaries" "$(find tools/linux/out/bin -maxdepth 1 -type f -perm -u+x | wc -l)"
    fi
    if [ "$TEST" = 1 ] && [ -f build/linux-test/serial.log ]; then
        printf '  %-32s %s\n' "Linux-ABI probe" \
            "$(grep -ao 'probe complete: [^=]*' build/linux-test/serial.log | tail -1 | sed 's/^probe complete: //; s/[[:space:]]*$//')"
    fi
}

# ── Build ─────────────────────────────────────────────────────────────────────
printf '%sAzamiOS complete build%s — %s, %s CPUs%s\n' "$B" "$N" \
    "$(make --no-print-directory -s doctor 2>/dev/null | sed -n 's/.*selected because: //p' | head -1)" \
    "${JOBS:-$(nproc)}" "$([ -n "${AZAMI_DEVENV:-}" ] && echo ', in the build container')"

if [ "$CLEAN" = 1 ]; then
    clean_all() {
        make "${MAKE_ARGS[@]}" clean
        make "${MAKE_ARGS[@]}" -C userland clean
        if [ "$DISTCLEAN" = 1 ]; then make "${MAKE_ARGS[@]}" linux-clean; fi
    }
    stage "Clean" clean_all
    # `make clean` removed build/, and the log with it; start a fresh one.
    mkdir -p "$LOG_DIR"
    echo "(clean stage output was written before build/ was removed)" > "$LOG"
fi

if [ "$NO_LINUX" = 0 ]; then
    stage "Stock Linux userland (musl, BusyBox, probe)" make "${MAKE_ARGS[@]}" linux
fi
if [ "$PORTS" = 1 ]; then
    # Some ports can fail on a given host (upstream moved, a missing tool) and
    # ports.mk already keeps going past them; the rest of the system does not
    # depend on any single port.
    STAGE_OPTIONAL=1 stage "Ported software" make "${MAKE_ARGS[@]}" -C tools/linux ports
fi
stage "Kernel, userland and disk image" make "${MAKE_ARGS[@]}" all
if [ "$NO_ISO" = 0 ]; then
    stage "Hybrid BIOS/UEFI ISO" make "${MAKE_ARGS[@]}" iso
fi
if [ "$TEST" = 1 ]; then
    if [ -x tools/linux/out/bin/azami-abi-probe ] || [ "$NO_LINUX" = 0 ]; then
        stage "Linux-ABI conformance probe" make "${MAKE_ARGS[@]}" linux-test
    else
        echo "  ⚠ --test needs the ABI probe, which --no-linux skipped building"
    fi
fi

summary

if [ -n "$RUN" ]; then
    echo
    printf '%s▸ Booting (%s)%s — close the QEMU window or press Ctrl-C here to stop\n' "$B" "$RUN" "$N"
    exec make "${MAKE_ARGS[@]}" "$RUN"
fi
