#!/usr/bin/env bash
# Host test for the firmware grouping (src/firmware.c).
#
# firmware.c's grouping is pure logic, so it is compiled with the host compiler
# and run against the real directory names of the linux-firmware mirror.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"
OUT="$(mktemp -d /tmp/cact-fw-test.XXXXXX)"
trap 'rm -rf "$OUT"' EXIT

CC="${CC:-cc}"
"$CC" -std=gnu17 -O1 -Wall -Wextra -Werror -I"$ROOT/src" \
    -o "$OUT/firmware_test" \
    "$ROOT/src/firmware.c" "$ROOT/test/firmware_test.c"

"$OUT/firmware_test"
