#!/bin/sh
# ============================================================================
# AzamiOS — uapi header sync check
#
# Verifies that the kernel and the native libc are still building against
# exactly one copy of the canonical syscall-number/errno headers
# (include/azami/uapi/{syscall_nr,errno}.h), rather than a second
# hand-maintained definition that could silently drift out of sync.
#
# Two things are checked:
#   1. Neither kernel/syscall/syscall.h nor the userland headers redefine a
#      SYS_*/E* name locally instead of #include-ing the canonical header —
#      a regression guard against someone "fixing a build error" by pasting
#      the old duplicated block back in.
#   2. The copy of the canonical headers staged into
#      userland/libc/include/azami/uapi/ (by `make -C userland uapi-sync`,
#      a prerequisite of libc/libc.a) is byte-identical to the source of
#      truth. A stale copy here means the userland build ran before the
#      canonical header's latest edit was picked up.
#
# Usage: scripts/check_uapi_sync.sh   (run from the repo root, or anywhere —
#        it locates the repo root from its own path)
# Exit status: 0 if in sync, 1 with a diagnostic otherwise.
# ============================================================================
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
FAIL=0

canonical_syscall="$ROOT/include/azami/uapi/syscall_nr.h"
canonical_errno="$ROOT/include/azami/uapi/errno.h"

for f in "$canonical_syscall" "$canonical_errno"; do
    if [ ! -f "$f" ]; then
        echo "check_uapi_sync: missing canonical header: $f" >&2
        FAIL=1
    fi
done

# ── 1. No consumer header may redefine a SYS_*/E* name locally ────────────
check_no_local_define() {
    header="$1"
    pattern="$2"
    label="$3"
    if [ -f "$header" ] && grep -Eq "$pattern" "$header"; then
        echo "check_uapi_sync: $header still hand-defines $label instead of" \
             "including the canonical uapi header — this is exactly the" \
             "drift Phase 0 removed; #include the canonical header instead." >&2
        FAIL=1
    fi
}

check_no_local_define "$ROOT/kernel/syscall/syscall.h" '^#define[[:space:]]+SYS_read[[:space:]]' "SYS_read"
check_no_local_define "$ROOT/include/azami/defs.h" '^#define[[:space:]]+EPERM[[:space:]]' "EPERM"
check_no_local_define "$ROOT/userland/libc/include/sys/syscall.h" '^#define[[:space:]]+SYS_read[[:space:]]' "SYS_read"
check_no_local_define "$ROOT/userland/libc/include/errno.h" '^#define[[:space:]]+EPERM[[:space:]]' "EPERM"

# Every consumer must actually #include the canonical header (directly, or
# via the staged copy) — catches the case where the block was simply deleted
# without adding the #include back.
check_has_include() {
    header="$1"
    pattern="$2"
    if [ -f "$header" ] && ! grep -Eq "$pattern" "$header"; then
        echo "check_uapi_sync: $header no longer includes the canonical" \
             "uapi header at all (pattern not found: $pattern)" >&2
        FAIL=1
    fi
}

check_has_include "$ROOT/kernel/syscall/syscall.h" 'include "\.\./\.\./include/azami/uapi/syscall_nr\.h"'
check_has_include "$ROOT/include/azami/defs.h" 'include "uapi/errno\.h"'
check_has_include "$ROOT/userland/libc/include/sys/syscall.h" 'include "\.\./azami/uapi/syscall_nr\.h"'
check_has_include "$ROOT/userland/libc/include/errno.h" 'include "azami/uapi/errno\.h"'

# ── 2. The staged userland copy must be byte-identical to the source ──────
staged_syscall="$ROOT/userland/libc/include/azami/uapi/syscall_nr.h"
staged_errno="$ROOT/userland/libc/include/azami/uapi/errno.h"

if [ -f "$staged_syscall" ] && [ -f "$canonical_syscall" ]; then
    if ! cmp -s "$staged_syscall" "$canonical_syscall"; then
        echo "check_uapi_sync: $staged_syscall is stale (differs from" \
             "$canonical_syscall) — run 'make -C userland uapi-sync'." >&2
        FAIL=1
    fi
elif [ ! -f "$staged_syscall" ]; then
    echo "check_uapi_sync: $staged_syscall not present yet — run" \
         "'make -C userland uapi-sync' (or a full userland build) at least" \
         "once before checking." >&2
    FAIL=1
fi

if [ -f "$staged_errno" ] && [ -f "$canonical_errno" ]; then
    if ! cmp -s "$staged_errno" "$canonical_errno"; then
        echo "check_uapi_sync: $staged_errno is stale (differs from" \
             "$canonical_errno) — run 'make -C userland uapi-sync'." >&2
        FAIL=1
    fi
elif [ ! -f "$staged_errno" ]; then
    echo "check_uapi_sync: $staged_errno not present yet — run" \
         "'make -C userland uapi-sync' (or a full userland build) at least" \
         "once before checking." >&2
    FAIL=1
fi

if [ "$FAIL" -eq 0 ]; then
    echo "check_uapi_sync: OK — kernel and userland agree on one canonical" \
         "uapi header set."
fi

exit "$FAIL"
