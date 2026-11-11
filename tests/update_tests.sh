#!/bin/sh
# The update path on the computer, against this build: its package through ota.c (step 1, in the running firmware)
# and the update loader (step 2: another build of this firmware -> this one, a damaged package, another board's,
# another chip key, an unknown flash chip). Run after ./build.sh.
#   tests/update_tests.sh [PACKAGE.fwsc APP.bin]
# STOCK_V15 / STOCK_V16: paths to the official FM-1.fwsc files (never in the repo), to check the return to stock.
set -e
cd "$(dirname "$0")/.."
PKG="${1:-$(ls build/*.fwsc | head -1)}"
APP="${2:-${PKG%.fwsc}.bin}"
OUT=build/tests
mkdir -p "$OUT"
cc -w -DOWN_PKG=1 -o "$OUT/ota_own" tests/ota_test.c
cc -w -o "$OUT/ota_stock" tests/ota_test.c
cc -w -o "$OUT/ldr_test" tests/ldr_test.c
echo "== step 1 (ota.c), this package: $PKG"
"$OUT/ota_own" "$PKG"
for v in V15 V16; do
    eval f=\${STOCK_$v:-}
    if [ -n "$f" ]; then
        echo "== step 1 (ota.c), the official $v: $f"
        "$OUT/ota_stock" "$f"
    else
        echo "== the official $v: skipped (STOCK_$v not set)"
    fi
done
echo "== step 2 (the loader): another build -> this one"
head -c 200000 "$APP" > "$OUT/old_app.bin"
python3 tools/fm1pkg_make.py "$OUT/old_app.bin" build/loader/ota.bin "$OUT/old.fwsc" > /dev/null
"$OUT/ldr_test" "$OUT/old.fwsc" "$PKG"
