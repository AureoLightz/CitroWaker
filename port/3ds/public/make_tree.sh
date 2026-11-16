#!/usr/bin/env bash
# (Claude, release prep) Builds the isolated asset-free source tree that `N3DS_PUBLIC=1 bash build.sh` compiles.
#
#   bash public/make_tree.sh            (re)create ~/ww-port/public-sw from the current ~/ww-port/SwitchWaker
#
# The production tree is never written. game/ and native/ are real copies (the runtime-assets tool rewrites sources
# there); build/native-linux is hard-linked (headers/compile metadata only, nothing writes it); build/assets/GZLE01/res
# (enum metadata) is copied; build/assets/GZLE01/include/assets gets the zero-content stubs instead of the disc-derived
# headers. Then tools/release/runtime-assets/prepare.py moves the 11 remaining inline payloads to runtime loading.
set -euo pipefail
SRC=$HOME/ww-port/SwitchWaker
DST=${PUBLIC_SW:-$HOME/ww-port/public/SwitchWaker}
TOOL=${RUNTIME_ASSETS_TOOL:-$(cd "$(dirname "$0")/../../.." && pwd)/tools/release/runtime-assets}
[ -f "$TOOL/prepare.py" ] || { echo "runtime-assets tool not found: $TOOL"; exit 1; }
case "$DST" in "$SRC"|"$SRC"/*) echo "refusing: destination inside the production tree"; exit 1;; esac

echo "== copy sources -> $DST"
mkdir -p "$DST"
rsync -a --delete --exclude .git --exclude /build "$SRC"/ "$DST"/
mkdir -p "$DST/build/assets/GZLE01"
rm -rf "$DST/build/native-linux"; cp -al "$SRC/build/native-linux" "$DST/build/native-linux"
rsync -a --delete "$SRC/build/assets/GZLE01/res/" "$DST/build/assets/GZLE01/res/"

echo "== stub asset headers"
rm -rf "$DST/build/assets/GZLE01/include"
python3 "$SRC/native/tools/gen_asset_stubs.py" --decomp "$SRC/build/decomp-docker" --version GZLE01 --out "$DST/build/assets/GZLE01"
n=$(ls "$DST/build/assets/GZLE01/include/assets"/*.h | wc -l)
bad=$(grep -L COS_ASSET_FILL "$DST/build/assets/GZLE01/include/assets"/*.h | grep -v '/cos_assets.h$' || true)
[ -z "$bad" ] || { echo "non-stub asset headers: $bad"; exit 1; }
echo "stub headers: $n"

echo "== inline payloads -> runtime loading"
python3 -I "$TOOL/prepare.py" --root "$DST"
python3 -I "$TOOL/prepare.py" --root "$DST" --apply
date -u +%FT%TZ > "$DST/.public-tree-stamp"
echo "public tree ready: $DST"
