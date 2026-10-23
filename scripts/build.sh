#!/usr/bin/env bash
# Builds the sysmodule and stages the SD layout in dist/sd/.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TID=420000000000BA7C

docker run --rm -v "$ROOT":/workspace -w /workspace devkitpro/devkita64:latest \
  bash -lc 'make clean >/dev/null && make -j4'

OUT="$ROOT/dist/sd/atmosphere/contents/$TID"
rm -rf "$ROOT/dist"
mkdir -p "$OUT/flags"
cp "$ROOT/bt-autoconnect.nsp" "$OUT/exefs.nsp"
: > "$OUT/flags/boot2.flag"
cat > "$OUT/toolbox.json" <<EOF
{
    "name": "BT Autoconnect",
    "tid": "$TID",
    "requires_reboot": true
}
EOF
shasum -a 256 "$OUT/exefs.nsp"
