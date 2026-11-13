#!/usr/bin/env python3
"""Stage inline disc payloads for runtime loading; defaults to a read-only plan.
Run only on an isolated source tree; base 163 stubs must already be generated.
"""
import argparse, hashlib, json, re
from pathlib import Path

def transform(root, specs):
    planned = {}
    def get(relative):
        if relative not in planned:
            planned[relative] = (root / relative).read_text()
        return planned[relative]
    for row in specs:
        name, symbol, rid = row['source'], row['symbol'], row['id']
        text = get(name)
        pattern = re.escape(symbol) + r'\s*\[[^\]]*\]\s*\)?\s*=\s*\{(.*?)\};'
        matches = list(re.finditer(pattern, text, re.S))
        if len(matches) != 1:
            raise ValueError(f'{name}: expected one original initializer for {symbol}')
        m = matches[0]
        body = re.sub(r'//[^\n]*|/\*.*?\*/', '', m[1], flags=re.S)
        values = re.findall(r'0x([0-9A-Fa-f]+)', body)
        raw = b''.join(int(v, 16).to_bytes(row['width'], 'big') for v in values)
        if len(raw) != row['bytes'] or hashlib.sha256(raw).hexdigest() != row['sha256']:
            raise ValueError(f'{name}: unsupported payload version for {symbol}')
        begin = text.rfind('\n', 0, m.start()) + 1
        prefix = text[begin:m.start()]
        indent = re.match(r'\s*', prefix)[0]
        typename = 'u32' if row['width'] == 4 else 'u8'
        linkage = 'static ' if 'static' in prefix else ''
        decl = f'{indent}{linkage}{typename} {symbol}[{row["bytes"]//row["width"]}] ATTRIBUTE_ALIGN(32);'
        if symbol == 'JUTResFONT_Ascfont_fix12':
            begin = text.index('#if TARGET_PC')
            decl = f'u8 {symbol}[{row["bytes"]}] ATTRIBUTE_ALIGN(32);'
        if symbol.startswith('JUTDirectPrint::'):
            ready = f'const bool JUTDirectPrint::public_asset_{rid}_ready'
        else:
            ready = f'{indent}static const bool public_asset_{rid}_ready'
        fill = f'{ready} = cos_asset_fill((void*)({symbol}),sizeof({symbol}),{rid});'
        planned[name] = text[:begin] + decl + '\n' + fill + text[m.end():]
    h = 'game/include/JSystem/JUtility/JUTFontData_Ascfont_fix12.h'
    text = get(h)
    if 'extern const u8' not in text:
        raise ValueError('font declaration changed; inspect before applying')
    planned[h] = text.replace('extern const u8', 'extern u8')
    h = 'game/include/JSystem/JUtility/JUTDirectPrint.h'
    text = get(h)
    anchor = '    static u8 sAsciiTable[128];'
    if text.count(anchor) != 1:
        raise ValueError('direct-print declaration changed; inspect before applying')
    declarations = ''.join(f'    static const bool public_asset_{rid}_ready;\n' for rid in (165,166,167))
    planned[h] = text.replace(anchor, declarations + anchor)
    table = 'build/assets/GZLE01/include/assets/cos_asset_table.inc'
    text = get(table)
    rows = [s for s in text.splitlines() if s.startswith('COS_ASSET_ROW')]
    if len(rows) != 163:
        raise ValueError('expected exactly 163 base runtime rows; refusing duplicate/shifted IDs')
    planned[table] = text + '\n// Additional inline payloads; no disc bytes in source.\n' + '\n'.join(r['row'] for r in specs) + '\n'
    return planned

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root',type=Path,required=True)
    p.add_argument('--apply',action='store_true',help='write only after every validation passes')
    a=p.parse_args()
    specs=json.loads(Path(__file__).with_name('payloads.json').read_text())
    planned=transform(a.root,specs)
    for name in sorted(planned): print(name)
    if a.apply:
        for name,text in planned.items():
            (a.root/name).write_text(text)
        print('Applied. Regenerate/rebuild all target objects with COS_RUNTIME_ASSETS=1; no mixed objects.')
    else: print('Plan only; no files written.')
if __name__=='__main__':main()
