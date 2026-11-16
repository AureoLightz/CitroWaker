#!/usr/bin/env python3
"""(Claude) Step 3 groundwork: every J3D display list stored on the disc (SHP1 shape geometry lists, MDL3 material lists), by content hash.
usage: dl_index.py GZLE01.iso out.tsv      lines: kind fnv64 size file#shape/group
"""
import struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import extract_textures as ex

def fnv(b):
    h = 0xCBF29CE484222325
    for c in b:
        h = ((h ^ c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h

class DlCollector(ex.Collector):
    def __init__(self):
        super().__init__(); self.dls = []
    def add(self, *a, **k):   # textures are not wanted here
        pass
    def j3d(self, b, name):
        u16 = lambda o: struct.unpack_from('>H', b, o)[0]; u32 = lambda o: struct.unpack_from('>I', b, o)[0]
        o = 0x20
        while o + 8 <= len(b):
            tag = bytes(b[o:o + 4]); size = u32(o + 4)
            if size < 8: break
            if tag == b'SHP1':
                n = u16(o + 8); init = o + u32(o + 0xC); dlbase = o + u32(o + 0x20); draw = o + u32(o + 0x28)
                for s in range(n):
                    si = init + s * 0x28
                    groups = u16(si + 2); di = u16(si + 8)
                    for g in range(groups):
                        dsz, doff = struct.unpack_from('>II', b, draw + (di + g) * 8)
                        d = bytes(b[dlbase + doff: dlbase + doff + dsz])
                        self.dls.append(('shape', fnv(d), dsz, '%s#%d/%d' % (name, s, g)))
            elif tag == b'MDL3':
                n = u16(o + 8); tab = o + u32(o + 0xC)
                for m in range(n):
                    e = tab + m * 8; doff, dsz = struct.unpack_from('>II', b, e)
                    d = bytes(b[e + doff: e + doff + dsz])
                    self.dls.append(('material', fnv(d), dsz, '%s#m%d' % (name, m)))
            o += size

def main():
    c = DlCollector()
    with open(sys.argv[1], 'rb') as f:
        entries, paths = ex.dm.read_fst(f)
        for i, e in enumerate(entries):
            if i == 0 or e['dir']: continue
            ln = e['name'].lower()
            if ln.endswith(('.thp', '.afc', '.aw', '.map', '.dds', '.rel', '.dol', '.aaf', '.bms', '.bmg')): continue
            f.seek(e['offset']); c.content(paths[i], f.read(e['size']))
    with open(sys.argv[2], 'w') as o:
        for k, h, n, nm in c.dls: o.write('%s\t%016x\t%d\t%s\n' % (k, h, n, nm))
    print(len(c.dls), 'display lists;', len({(h, n) for _, h, n, _ in c.dls}), 'unique')

if __name__ == '__main__':
    main()
