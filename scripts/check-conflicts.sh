#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd "$SCRIPT_DIR/.." && pwd)
cd "$REPO_ROOT"

PATTERN='^(<<<<<<<|=======|>>>>>>>)'

# Search the checked-out files directly instead of relying on Git metadata.
# This keeps builds from ZIP archives and folders under Downloads working.
conflicts=$(
    find bootloader kernel \
        -type f \
        \( -name '*.asm' -o -name '*.c' -o -name '*.h' -o -name '*.ld' \) \
        -exec grep -Hn -E "$PATTERN" {} + 2>/dev/null || true
    grep -Hn -E "$PATTERN" Makefile 2>/dev/null || true
)

if [ -n "$conflicts" ]; then
    cat <<'MSG' >&2
Error: Git conflict markers were detected in the source tree.
Please resolve the conflicts listed below before building.
MSG
    printf '%s\n' "$conflicts" >&2
    exit 1
fi
