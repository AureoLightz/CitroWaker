#!/usr/bin/env bash
# (Claude, release prep) Packages the asset-free build (N3DS_PUBLIC=1 bash build.sh -> public-build/ww3ds.elf) as
# public-build/CitroWaker.cia with original HOME resources only: the open-ring emblem banner/icon made by
# tools/release/original-home/generate.py (no sword, Triforce, game texture or font), the oscillator-synthesised highlight
# chime (presentation/home_audio, no game audio), and an RSF without the Nintendo boot logo. Never touches cia/ww3ds.cia.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
P=$here/public-build; T=$here/cia/tools
ORIG=${ORIGINAL_HOME:-$here/../../tools/release/original-home}
[ -s "$P/ww3ds.elf" ] || { echo "no public ELF: N3DS_PUBLIC=1 bash build.sh"; exit 1; }
mkdir -p "$P/home"
python3 -I "$ORIG/generate.py" --output "$P/home" --font "${HOME_FONT:-/usr/share/fonts/google-noto/NotoSerif-Bold.ttf}" >/dev/null   # SIL OFL font
audio=$here/presentation/home_audio/citrowaker-highlight.wav
[ -s "$audio" ] || python3 -I "$here/presentation/home_audio/generate.py" >/dev/null   # synthesized; not stored in git
cp "$P/ww3ds.elf" "$P/ww3ds.snapshot.elf"
# (Claude v133) own title (UniqueId 0xff2d8, product code CTR-P-CTWK): installs beside the developer build instead of replacing it
grep -v '^ *Logo *:' "$here/cia/ww3ds.rsf" | sed -E 's/^( *UniqueId *: *).*/\10xff2d8/; s/^( *ProductCode *: *).*/\1CTR-P-CTWK/; s/^( *Title *: *).*/\1CitroWaker/' > "$P/ww3ds-public.rsf"
cd "$P"
"$T/bannertool" makebanner -i home/banner.png -a "$audio" -o banner.bnr
"$T/bannertool" makesmdh -s 'CitroWaker' -l 'CitroWaker' -p 'CitroWaker contributors' -i home/icon-48.png -o icon.smdh
"$T/makerom" -f cia -o CitroWaker.cia.new -elf ww3ds.snapshot.elf -rsf ww3ds-public.rsf -icon icon.smdh -banner banner.bnr -target t
"$T/ctrtool" --exheader=exheader.bin CitroWaker.cia.new > inspection.txt 2>&1
grep -Eq 'System mode \(New3DS\):.*124MB' inspection.txt || { echo "exheader is not New 3DS 124MB mode; see $P/inspection.txt"; exit 1; }
mv CitroWaker.cia.new CitroWaker.cia
sha256sum CitroWaker.cia | tee CitroWaker.cia.sha256
