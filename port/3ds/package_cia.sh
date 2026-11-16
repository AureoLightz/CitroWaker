#!/usr/bin/env bash
# Package an existing ELF only. Never builds/relinks or reads game data.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cd "$here/cia"
exec 8>.package.lock
flock 8
for tool in makerom bannertool ctrtool; do
  [[ -x tools/$tool ]] || { echo "Missing cia/tools/$tool (see cia/README.md)" >&2; exit 1; }
done
# Read a stable inode while another agent may be replacing/relinking the ELF.
python3 - "$here/ww3ds.elf" "$PWD/ww3ds.snapshot.elf" <<'PY'
from pathlib import Path
import os,sys,hashlib,json,time
source,target=map(Path,sys.argv[1:]);temp=target.with_suffix('.tmp')
for attempt in range(5):
    with source.open('rb') as f:
        before=os.fstat(f.fileno());data=f.read();after=os.fstat(f.fileno())
    current=source.stat()
    def signature(s):return s.st_ino,s.st_size,s.st_mtime_ns
    if signature(before)==signature(after)==signature(current) and len(data)==before.st_size and data[:4]==b'\x7fELF':
        temp.write_bytes(data);temp.replace(target)
        Path('snapshot.json').write_text(json.dumps({'source':str(source),'bytes':len(data),'source_mtime_ns':before.st_mtime_ns,'sha256':hashlib.sha256(data).hexdigest()},indent=2)+'\n')
        break
    time.sleep(.2)
else:raise SystemExit('ELF changed during snapshot; rerun packaging once the current link finishes')
PY
# (Claude v102) ChatGPT's CitroWaker banner (Master Sword + wordmark, presentation/citrowaker_home/local/banner.png) and Triforce icon (icon-48.png), original synthesized highlight (no game audio).
audio="$here/presentation/home_audio/citrowaker-highlight.wav"
[[ -s banner.png && -s "$audio" ]] || { echo "Missing presentation assets; see presentation/README.md" >&2; exit 1; }
# (Claude 2026-11-10) The CGFX model banner crashed the real HOME Menu (Luma dump: process "menu", data abort) when the icon was
# selected. Image banner (bannertool builds a standard CGFX from it) and the highlight cut to 2.9 s (the HOME Menu's banner sound limit is 3 s).
tools/bannertool makebanner -i banner.png -a "$audio" -o banner.bnr
tools/bannertool makesmdh -s 'CitroWaker' -l 'CitroWaker' -p 'CitroWaker contributors' -i icon.png -o icon.smdh
tools/makerom -f cia -o ww3ds.cia.new -elf ww3ds.snapshot.elf -rsf ww3ds.rsf -icon icon.smdh -banner banner.bnr -target t
tools/ctrtool --exheader=ww3ds.exheader.bin ww3ds.cia.new > inspection.txt 2>&1
if ! grep -E 'System mode \(New3DS\):.*124MB' inspection.txt >/dev/null; then
  echo 'Packaged exheader did not report New 3DS 124MB mode; see cia/inspection.txt' >&2
  exit 1
fi
mv -- ww3ds.cia.new ww3ds.cia
sha256sum ww3ds.cia > ww3ds.cia.sha256
printf 'Packaged %s/cia/ww3ds.cia (ISO and material bundles remain external)\n' "$here"
