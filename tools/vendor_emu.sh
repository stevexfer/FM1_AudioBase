#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Copy the browser emulator into web/emu/ (web/make_site.py serves it as emu/, with this firmware
# in its catalog), built from a checkout of FM-1Ception (MIT: a fork of Joakim Eriksson's fm1-emu)
# with its USB audio in. Needs rustup's stable toolchain with the wasm32-unknown-unknown target.
#   tools/vendor_emu.sh ../FM-1Ception
set -euo pipefail
EMU="$(cd "${1:?usage: tools/vendor_emu.sh FM-1Ception-checkout}" && pwd)"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/web/emu"
DIST="$(mktemp -d)"
trap 'rm -rf "$DIST"' EXIT
"$EMU/web/build.sh" "$DIST" >/dev/null
rm -rf "$OUT"
mkdir -p "$OUT"
cp "$DIST"/{fm1_web.wasm,index.html,style.css,main.js,persistence.js,worklet.js,jit.js,fwsc.js} "$OUT/"
cp -R "$DIST/kalimba" "$OUT/"
cp "$EMU/LICENSE" "$OUT/LICENSE"
echo "FM-1Ception $(git -C "$EMU" rev-parse HEAD)$(git -C "$EMU" diff --quiet HEAD || echo ' (with local changes)')" > "$OUT/VERSION"
cat "$OUT/VERSION"
du -sh "$OUT"
