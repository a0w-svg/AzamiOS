#!/usr/bin/env bash
# Run a command inside the AzamiOS build container.
#
#   scripts/devenv.sh                 interactive shell in the build environment
#   scripts/devenv.sh make            build the kernel + disk image
#   scripts/devenv.sh make run        build and boot (QEMU window via host X11/Wayland)
#   scripts/devenv.sh --rebuild       rebuild the image (after editing the Containerfile)
#
# Uses podman if present, docker otherwise. The image is built on first use
# from tools/devenv/Containerfile and tagged with a hash of that file, so an
# edited Containerfile is picked up automatically without --rebuild.
#
# The source tree is mounted at its host path and the command runs as the
# host user, so every file the build writes is owned by you and absolute paths
# embedded in objects (DWARF, .d files) are valid on the host too — `make gdb`
# on the host can read a kernel built in the container.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
CONTAINERFILE=$ROOT/tools/devenv/Containerfile

if command -v podman >/dev/null 2>&1; then
    ENGINE=podman
elif command -v docker >/dev/null 2>&1; then
    ENGINE=docker
else
    echo "devenv: neither podman nor docker is installed" >&2
    echo "        install one, or install the packages listed in $CONTAINERFILE on the host" >&2
    exit 1
fi

HASH=$(sha256sum "$CONTAINERFILE" | cut -c1-12)
IMAGE=localhost/azamios-devenv:$HASH

if [ "${1:-}" = "--rebuild" ]; then
    shift
    "$ENGINE" rmi -f "$IMAGE" >/dev/null 2>&1 || true
fi

if ! "$ENGINE" image exists "$IMAGE" 2>/dev/null &&
   ! "$ENGINE" image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "devenv: building $IMAGE (one-time, a few minutes)..." >&2
    "$ENGINE" build -t "$IMAGE" -f "$CONTAINERFILE" "$ROOT/tools/devenv"
fi

# Stay inside the tree when invoked from outside it.
case "$PWD" in "$ROOT"|"$ROOT"/*) WORKDIR=$PWD ;; *) WORKDIR=$ROOT ;; esac

# SELinux: the container runs unconfined (label=disable) rather than
# relabelling the tree with :z. Relabelling would rewrite the security
# context of your checkout to container_file_t, and could not reach the X11,
# Wayland or PulseAudio sockets mounted below at all — a socket owned by the
# host session cannot be relabelled, so `make run` would have no display and
# no sound. toolbox and distrobox make the same choice.
args=(run --rm --init
      --security-opt label=disable
      -v "$ROOT:$ROOT"
      -v azamios-ccache:/ccache
      -w "$WORKDIR"
      -e HOME="$ROOT/build/.home"
      -e TERM="${TERM:-xterm}"
      -e MAKEFLAGS="${MAKEFLAGS:-}")

if [ "$ENGINE" = podman ]; then
    # keep-id maps the container user onto the host uid, so build outputs are
    # not owned by a subordinate uid the host user cannot delete.
    args+=(--userns=keep-id)
else
    args+=(--user "$(id -u):$(id -g)")
fi

# Hardware acceleration and a display for `make run`, when the host has them.
[ -e /dev/kvm ] && args+=(--device /dev/kvm)
[ -n "${DISPLAY:-}" ] && [ -d /tmp/.X11-unix ] &&
    args+=(-e DISPLAY -v /tmp/.X11-unix:/tmp/.X11-unix)
if [ -n "${WAYLAND_DISPLAY:-}" ] && [ -n "${XDG_RUNTIME_DIR:-}" ] &&
   [ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]; then
    args+=(-e WAYLAND_DISPLAY -e XDG_RUNTIME_DIR=/run/user/host
           -v "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY:/run/user/host/$WAYLAND_DISPLAY")
fi
# PulseAudio / PipeWire-pulse socket for the emulated sound cards.
if [ -n "${XDG_RUNTIME_DIR:-}" ] && [ -S "$XDG_RUNTIME_DIR/pulse/native" ]; then
    args+=(-e PULSE_SERVER=unix:/run/pulse/native
           -v "$XDG_RUNTIME_DIR/pulse/native:/run/pulse/native")
fi
[ -t 0 ] && [ -t 1 ] && args+=(-it)

mkdir -p "$ROOT/build/.home"

if [ $# -eq 0 ]; then
    set -- bash
fi
exec "$ENGINE" "${args[@]}" "$IMAGE" "$@"
