#!/usr/bin/env bash
# Host test for the pure TUI layer (src/tui_wire.c).
#
# The installer is a freestanding CactLibc PIE and cannot run on the dev host,
# so the frame layout and key decoder are compiled with the host compiler and
# checked directly — the same split FAT32/test/run_test.sh uses for its module.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"
OUT="$(mktemp -d /tmp/cact-tui-wire.XXXXXX)"
trap 'rm -rf "$OUT"' EXIT

CC="${CC:-cc}"
"$CC" -std=gnu17 -O1 -Wall -Wextra -Werror -I"$ROOT/src" \
    -o "$OUT/tui_wire_test" \
    "$ROOT/src/tui_wire.c" "$ROOT/test/tui_wire_test.c"

"$OUT/tui_wire_test"
