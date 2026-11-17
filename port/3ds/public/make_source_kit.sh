#!/usr/bin/env bash
# (Claude v136) Complete corresponding source for the public CitroWaker.cia (GPL-2.0-or-later via native/dsp_hle + Dolphin DSPHLE).
#
#   bash public/make_source_kit.sh TRACE OUT_DIR
#
# TRACE is an strace log of a full public build (build-trace.sh: N3DS_PUBLIC=1 N3DS_PGO=use, every unit rebuilt), so the kit
# holds every file the compilers and linker actually read under /work (public SwitchWaker tree), /arm (this folder) and /hw6
# (renderer), plus the host-side inputs build.sh reads before the container starts (compile database, Aurora sources it patches,
# generators, PGO profile) and the CIA packaging script and its inputs. The kit rebuilds the same ww3ds.elf (check: sha256).
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
TRACE=$1; OUT=$2
SW=${N3DS_SW:-$HOME/ww-port/public/SwitchWaker}
HW6=${N3DS_HW6:-$HOME/Downloads/ww-replay3ds/hw6}
ORIG=${ORIGINAL_HOME:-$here/../../tools/release/original-home}
[ ! -e "$OUT" ] || { echo "refusing: $OUT exists"; exit 1; }
mkdir -p "$OUT/SwitchWaker" "$OUT/n3ds" "$OUT/hw6"
list=$(mktemp)
python3 - "$TRACE" "$list" "$SW" "$here" "$HW6" <<'EOF'
import os, re, sys
trace, out, sw, arm, hw6 = sys.argv[1:]
roots = {'/work': sw, '/arm': arm, '/hw6': hw6}
skip_arm = ('/arm/obj/', '/arm/obj3ds/', '/arm/log/', '/arm/bench/', '/arm/cia/ww3ds', '/arm/public-build/')
skip_names = {'/arm/all.sh', '/arm/todo.sh', '/arm/link.log', '/arm/ww3ds.elf', '/arm/ww3ds.3dsx', '/arm/gen/trace.txt'}
pat = re.compile(r'openat\([^,]+, "([^"]+)", ([A-Z_|]+)')
files = set()
for line in open(trace, errors='replace'):
    m = pat.search(line)
    if not m or '= -1' in line: continue
    p, flags = m.group(1), m.group(2)
    if 'O_WRONLY' in flags or 'O_RDWR' in flags or 'O_DIRECTORY' in flags or not p.startswith('/'): continue
    p = os.path.normpath(p); parts = p.split('/')
    if len(parts) < 3 or '/' + parts[1] not in roots: continue
    if p in skip_names or p.startswith(skip_arm): continue
    top = '/' + parts[1]; host = roots[top] + p[len(top):]
    if os.path.isfile(host): files.add(top[1:] + '\t' + p[len(top) + 1:])
open(out, 'w').write('\n'.join(sorted(files)) + '\n')
print(len(files), 'traced files')
EOF
copy() {  # copy() ROOT REL DESTROOT
  mkdir -p "$3/$(dirname "$2")"; cp -p "$1/$2" "$3/$2"
}
while IFS=$'\t' read -r top rel; do
  case $top in work) copy "$SW" "$rel" "$OUT/SwitchWaker";; arm) copy "$here" "$rel" "$OUT/n3ds";; hw6) copy "$HW6" "$rel" "$OUT/hw6";; esac
done < "$list"
rm -f "$list"

# host-side inputs of build.sh (read before the container starts) and whole small source folders
copy "$SW" build/native-linux/compile_commands.json "$OUT/SwitchWaker"
mkdir -p "$OUT/SwitchWaker/build/native-linux/_deps"
cp -a "$SW/build/native-linux/_deps/aurora-src" "$OUT/SwitchWaker/build/native-linux/_deps/"
cp -p "$SW/.public-tree-stamp" "$OUT/SwitchWaker/"
for f in LICENSE LICENSE.md README.md THIRD_PARTY.md native/dsp_hle/LICENSE native/dsp_hle/README.md; do [ -f "$SW/$f" ] && copy "$SW" "$f" "$OUT/SwitchWaker"; done
for f in COPYING README.md; do [ -f "$SW/ref/recompcore/$f" ] && copy "$SW" "ref/recompcore/$f" "$OUT/SwitchWaker"; done
cp -a "$SW/ref/recompcore/LICENSES" "$OUT/SwitchWaker/ref/recompcore/"
for f in build.sh mkcmds.py gen_typed_gx.py runone.sh exclude.txt Containerfile pgo/ww.profdata package_cia.sh cia/ww3ds.rsf \
         public/package_public.sh presentation/home_audio/generate.py; do
  [ -f "$here/$f" ] && copy "$here" "$f" "$OUT/n3ds"
done
rsync -a --exclude '*.o' "$here/src/" "$OUT/n3ds/src/"
rsync -a --exclude '*.o' --exclude build "$here/citro3d/" "$OUT/n3ds/citro3d/"
rsync -a --exclude '*.o' "$HW6/src/" "$OUT/hw6/src/"; rsync -a --exclude '*.o' "$HW6/src3ds/" "$OUT/hw6/src3ds/"
mkdir -p "$OUT/original-home"; rsync -a --exclude __pycache__ "$ORIG/" "$OUT/original-home/"
# kit-relative defaults instead of this machine's folders
sed -i 's#^ORIG=.*#ORIG=${ORIGINAL_HOME:-$here/../original-home}#' "$OUT/n3ds/public/package_public.sh"
sed -i 's#^SW=\${N3DS_SW:-$HOME/ww-port/SwitchWaker}#SW=${N3DS_SW:-$here/../SwitchWaker}#; s#^  SW=\${N3DS_SW:-$HOME/ww-port/public/SwitchWaker}#  SW=${N3DS_SW:-$here/../SwitchWaker}#; s#^HW6=\${N3DS_HW6:-$HOME/Downloads/ww-replay3ds/hw6}#HW6=${N3DS_HW6:-$here/../hw6}#' "$OUT/n3ds/build.sh"
if grep -rIl "$HOME" "$OUT"; then echo "kit contains this machine's paths (above)"; exit 1; fi

# include paths like xxhash-src/cmake_unofficial/.. resolve only when the (otherwise unused) folder exists
python3 - "$OUT/SwitchWaker" <<'PYEOF'
import json, os, re, shlex, sys
root = sys.argv[1]
cc = json.load(open(os.path.join(root, 'build/native-linux/compile_commands.json')))
dirs = set()
for e in cc:
    for m in re.finditer(r'(?:-I|-isystem |-iquote |-idirafter )(/work/[^ ]+)', e['command']):
        d = m.group(1)
        while '/..' in d:
            d = d[:d.index('/..')]
        dirs.add(d)
for d in sorted(dirs):
    os.makedirs(os.path.join(root, d[len('/work/'):]), exist_ok=True)
print(len(dirs), 'include folders')
PYEOF
# nothing private may leave: no disc-derived asset header (only zero-content stubs), no user paths
bad=$(grep -rL --include='*.h' COS_ASSET_FILL "$OUT/SwitchWaker/build/assets/GZLE01/include/assets" 2>/dev/null | grep -v '/cos_assets.h$' || true)
[ -z "$bad" ] || { echo "non-stub asset headers in kit: $bad"; exit 1; }
cp "$here/public/SOURCE_KIT_README.md" "$OUT/README.md"
[ -n "${SOURCE_DATE_EPOCH:-}" ] && echo "$SOURCE_DATE_EPOCH" > "$OUT/SOURCE_DATE_EPOCH"   # set for the published build
[ -s "$here/public-build/ww3ds.elf" ] && (cd "$here/public-build" && sha256sum ww3ds.elf) > "$OUT/ELF.sha256"
( cd "$OUT" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS )
echo "kit: $OUT ($(find "$OUT" -type f | wc -l) files, $(du -sh "$OUT" | cut -f1))"
