#!/usr/bin/env python3
"""Find known embedded game-asset bytes in an ELF. Not a legal/distribution clearance.
Inputs stay local. Exit 1 means known payload found; exit 2 means invalid input.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

def raw_headers(folder):
    for path in sorted(Path(folder).rglob('*.h')):
        text=path.read_text(errors='replace')
        for number,m in enumerate(re.finditer(r'\{([^{}]*)\}',text,re.S)):
            body=re.sub(r'/\*.*?\*/|//[^\n]*','',m[1],flags=re.S)
            tokens=re.findall(r'0[xX]([0-9A-Fa-f]+)',body)
            # Only raw-byte initializers; do not guess how C floats were emitted.
            if len(tokens)<32 or any(len(t)>2 for t in tokens):continue
            if re.sub(r'0[xX][0-9A-Fa-f]+|[\s,\\]','',body):continue
            payload=bytes(int(t,16)for t in tokens)
            if len(set(payload))<8:continue # all-zero/common fill is not reliable evidence
            yield path.relative_to(folder).as_posix(),number,payload

def inspect(elf,asset_headers):
    elf=Path(elf);data=elf.read_bytes()
    if data[:4]!=b'\x7fELF':raise ValueError('Supply the uncompressed ELF paired with the candidate CIA, not the compressed CIA itself.')
    hits=[];candidates=0
    for name,number,payload in raw_headers(asset_headers):
        candidates+=1;at=data.find(payload)
        if at>=0:hits.append({'header':name,'initializer':number,'bytes':len(payload),'elf_offset':at,'sha256':hashlib.sha256(payload).hexdigest()})
    return {'elf_sha256':hashlib.sha256(data).hexdigest(),'known_raw_arrays_checked':candidates,'embedded_matches':hits,'known_raw_payload_found':bool(hits),'scope':'Known raw-byte arrays only; no clearance of float assets, UI art, music, firmware or source/licence completeness.'}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--elf',type=Path,required=True);p.add_argument('--asset-headers',type=Path,required=True);p.add_argument('--output',type=Path);a=p.parse_args()
    try:
        if not a.asset_headers.is_dir():raise ValueError('Asset header directory is missing.')
        report=inspect(a.elf,a.asset_headers);text=json.dumps(report,indent=2)+'\n'
        if a.output:
            with a.output.open('x',encoding='utf-8')as f:f.write(text)
        print(f"Checked {report['known_raw_arrays_checked']} known raw arrays; {len(report['embedded_matches'])} exact ELF matches.")
        if not report['known_raw_arrays_checked']:raise ValueError('No supported arrays found; nothing was audited.')
        return 1 if report['known_raw_payload_found']else 0
    except (ValueError,OSError)as e:p.exit(2,str(e)+'\n')
if __name__=='__main__':raise SystemExit(main())
