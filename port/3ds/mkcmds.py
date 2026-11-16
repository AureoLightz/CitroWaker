# converts compile_commands.json entries (clang, x86_64 Linux) into devkitARM g++ commands
import json, re, sys, shlex
import os
cc = json.load(open(os.path.join(os.environ.get('SW_ROOT') or os.path.expanduser('~/ww-port/SwitchWaker'), 'build/native-linux/compile_commands.json')))
public = os.environ.get('N3DS_PUBLIC') == '1'   # (Claude, release prep) asset-free build: data arrays loaded from the ISO at start
pat = sys.argv[1]; out = sys.argv[2]
drop = {'-fcolor-diagnostics', '-fPIC', '-g'}
lines = []
seen = set()
for e in cc:
    f = e['file']
    if not re.search(pat, f): continue
    if '-fsanitize' in e['command'] or 'cos_sdk_smoke' in e['command'] or 'cos_pc_tests' in e['command'] or 'layout_check' in e['command'] or 'header_check' in e['command'] or 'scaffold_check' in e['command']: continue
    if f in seen: continue
    seen.add(f)
    args = shlex.split(e['command'])
    cxx = args[0].endswith('++')
    D = '/opt/devkitpro/devkitARM/arm-none-eabi/include'
    na = ['clang++' if cxx else 'clang', '--target=armv6k-none-eabihf', '-mcpu=mpcore', '-mfloat-abi=hard', '-mfpu=vfpv2', '-mtp=soft', '-nostdlibinc', '-D__3DS__', '-D_GNU_SOURCE'] + (['-DCOS_RUNTIME_ASSETS=1'] if public else [])
    if cxx: na += ['-isystem', D + '/c++/16.1.0', '-isystem', D + '/c++/16.1.0/arm-none-eabi/armv6k/fpu', '-isystem', D + '/c++/16.1.0/backward']
    na += ['-isystem', D, '-isystem', '/opt/devkitpro/libctru/include', '-idirafter', '/opt/devkitpro/portlibs/3ds/include', '-include', '/work/native/include/pc/cos_3ds_config.h']
    skip = False
    for i, a in enumerate(args[1:]):
        if skip: skip = False; continue
        if a in drop or a.startswith('-ferror-limit'): continue
        if a == '-o': na += ['-o', '/arm/obj/' + re.sub(r'[^A-Za-z0-9_.]', '_', f.split('/work/')[-1]) + '.o']; skip = True; continue
        if a in ('-D_ARCH_64=1', '-D_M_X86=1', '-D_M_X86_64=1', '-D_M_ARM_64=1'):
            a = {'-D_ARCH_64=1': '-D_ARCH_32=1', '-D_M_X86=1': '-D_M_ARM_32=1'}.get(a)
            if a is None: continue
        na.append(a)
    na += ['-w', '-ferror-limit=30']
    lines.append(' '.join(shlex.quote(x) for x in na))
open(out, 'w').write('\n'.join(lines) + '\n')
print(len(lines), 'commands')
