#!/usr/bin/env python3
"""(Claude) Texture pack step 1: every texture stored on the Wind Waker disc, as raw GX data, for mktexpack.

usage: extract_textures.py GZLE01.iso texsrc.bin [--list list.tsv]

Walks the disc (FST), Yaz0, RARC archives (nested), and collects the ResTIMG (BTI) images of
  * .bti files,
  * J3D files (BMD/BDL/BMT...) TEX1 blocks,
  * JPC particle archives (TEX1 chunks).
Identical images (same header fields that matter + same bytes + same palette) are written once.

texsrc.bin: "WWTS" u32 version 1, u32 count, then per texture:
  u8 fmt, u8 wrapS, u8 wrapT, u8 mipCount, u16 w, u16 h, u8 tlutFmt, u8 mipmapFlag, s8 minLod, s8 maxLod,
  u16 paletteEntries, u16 pad, u32 imageBytes, u32 paletteBytes, u32 nameLen, image, palette (raw big-endian GX entries), name
Uses SwitchWaker/native/tools/disc_manifest.py for the container formats.
"""
import argparse, struct, sys, hashlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / 'third_party' / 'switchwaker'))
import disc_manifest as dm  # noqa: E402

# GX texture formats: (block width, block height, bytes per block)
BLOCK = {0: (8, 8, 32), 1: (8, 4, 32), 2: (8, 4, 32), 3: (4, 4, 32), 4: (4, 4, 32), 5: (4, 4, 32), 6: (4, 4, 64),
         8: (8, 8, 32), 9: (8, 4, 32), 10: (4, 4, 32), 14: (8, 8, 32)}


def level_bytes(fmt, w, h):
    bw, bh, bb = BLOCK[fmt]
    return ((w + bw - 1) // bw) * ((h + bh - 1) // bh) * bb


def image_bytes(fmt, w, h, levels):
    n = 0
    for l in range(levels):
        n += level_bytes(fmt, max(1, w >> l), max(1, h >> l))
    return n


class Collector:
    def __init__(self):
        self.seen = {}
        self.records = []
        self.stats = {'bti': 0, 'j3d': 0, 'jpc': 0, 'dup': 0, 'bad': 0}

    def add(self, b, hdr, name, kind):
        """b: the buffer the header offsets are relative to start at hdr (BTI convention: offsets from the header)."""
        try:
            fmt = b[hdr]; w, h = struct.unpack_from('>HH', b, hdr + 2)
            ws, wt = b[hdr + 6], b[hdr + 7]
            tlut_fmt = b[hdr + 9]; ncol = struct.unpack_from('>H', b, hdr + 0xA)[0]
            pal_off = struct.unpack_from('>I', b, hdr + 0xC)[0]
            mip_flag = b[hdr + 0x10]
            min_lod, max_lod = struct.unpack_from('>bb', b, hdr + 0x16)
            mips = max(1, b[hdr + 0x18])
            img_off = struct.unpack_from('>I', b, hdr + 0x1C)[0]
        except (struct.error, IndexError):
            self.stats['bad'] += 1; return
        if fmt not in BLOCK or not (1 <= w <= 1024 and 1 <= h <= 1024):
            self.stats['bad'] += 1; return
        nimg = image_bytes(fmt, w, h, mips)
        img = bytes(b[hdr + img_off: hdr + img_off + nimg])
        if len(img) < nimg:   # some files store fewer levels than they claim: keep what is there
            while mips > 1 and image_bytes(fmt, w, h, mips) > len(img):
                mips -= 1
            nimg = image_bytes(fmt, w, h, mips)
            if len(img) < nimg:
                self.stats['bad'] += 1; return
            img = img[:nimg]
        pal = b''
        if fmt in (8, 9, 10):
            pal = bytes(b[hdr + pal_off: hdr + pal_off + 2 * ncol])
            if len(pal) < 2 * ncol:
                self.stats['bad'] += 1; return
        key = hashlib.sha1(bytes([fmt, ws, wt, mips, tlut_fmt, mip_flag, min_lod & 255, max_lod & 255]) +
                           struct.pack('>HHH', w, h, ncol) + img + pal).digest()
        if key in self.seen:
            self.stats['dup'] += 1; return
        self.seen[key] = len(self.records)
        self.stats[kind] += 1
        self.records.append((fmt, ws, wt, mips, w, h, tlut_fmt, mip_flag, min_lod, max_lod, ncol if pal else 0, img, pal, name))

    def j3d(self, b, name):
        o = 0x20
        while o + 8 <= len(b):
            tag = bytes(b[o:o + 4]); size = struct.unpack_from('>I', b, o + 4)[0]
            if size < 8:
                break
            if tag == b'TEX1':
                n = struct.unpack_from('>H', b, o + 8)[0]
                hoff = struct.unpack_from('>I', b, o + 0xC)[0]
                for i in range(n):
                    self.add(b, o + hoff + i * 0x20, '%s#%d' % (name, i), 'j3d')
            o += size

    def jpc(self, b, name):
        try:
            rec = dm.parse_jpc(b)
        except Exception:
            self.stats['bad'] += 1; return
        # walk again to the texture chunks (parse_jpc does not return offsets)
        o = 0x20
        for _ in range(rec['emitter_count']):
            nblocks = struct.unpack_from('>I', b, o + 0xC)[0]
            bo = o + 0x20
            for _ in range(nblocks):
                bo += struct.unpack_from('>I', b, bo + 4)[0]
            o = bo
        for i in range(rec['texture_count']):
            size = struct.unpack_from('>I', b, o + 4)[0]
            self.add(b, o + 0x20, '%s#%s' % (name, rec['textures'][i]['name']), 'jpc')
            o += size

    def content(self, name, b):
        if b[:4] == b'Yaz0':
            try:
                b = dm.yaz0_decompress(b)
            except Exception:
                return
        head = bytes(b[:8])
        if head[:4] == b'RARC':
            self.rarc(name, b)
        elif head[:4] == b'J3D2' or head[:4] == b'J3D1':
            self.j3d(b, name)
        elif head == b'JPAC1-00':
            self.jpc(b, name)
        elif name.lower().endswith('.bti'):
            self.add(b, 0, name, 'bti')

    def rarc(self, where, b):
        u32 = lambda o: struct.unpack_from('>I', b, o)[0]
        info = 0x20
        num_entries = u32(info + 8); entry_off = info + u32(info + 0xC); str_off = info + u32(info + 0x14)
        data_base = 0x20 + u32(0xC)
        for i in range(num_entries):
            o = entry_off + i * 0x14
            tf = u32(o + 4)
            if (tf >> 24) & 2:
                continue
            nm = dm.cstr(b, str_off + (tf & 0xFFFFFF))
            start = data_base + u32(o + 8); size = u32(o + 0xC)
            self.content('%s:%s' % (where, nm), b[start:start + size])


def main():
    ap=argparse.ArgumentParser(description='Extract texture records from your USA revision-0 ISO. No existing output is replaced.')
    ap.add_argument('iso',type=Path);ap.add_argument('output',type=Path);ap.add_argument('--list',dest='listing',type=Path)
    args=ap.parse_args();iso=args.iso.resolve();out=args.output.absolute()
    outputs=[out]+([args.listing.absolute()]if args.listing else [])
    if len(set(outputs))!=len(outputs):ap.error('Texture output and listing must be different files.')
    if any(p.exists()or p.resolve()==iso for p in outputs):ap.error('Choose NEW output/listing paths; input and existing files are never replaced.')
    try:
        with iso.open('rb')as source:header=source.read(0x440)
    except OSError as error:ap.error(str(error))
    if len(header)<0x440 or header[:6]!=b'GZLE01' or header[7]!=0 or header[0x1c:0x20]!=bytes.fromhex('c2339f3d'):
        ap.error('A USA revision-0 GameCube ISO (GZLE01) is required.')
    out.parent.mkdir(parents=True,exist_ok=True)
    c = Collector()
    with open(iso, 'rb') as f:
        entries, paths = dm.read_fst(f)
        for i, e in enumerate(entries):
            if i == 0 or e['dir']:
                continue
            ln = e['name'].lower()
            if ln.endswith(('.thp', '.afc', '.aw', '.map', '.dds', '.rel', '.dol', '.aaf', '.bms', '.bmg')):
                continue
            f.seek(e['offset'])
            c.content(paths[i], f.read(e['size']))
    with out.open('xb') as o:
        o.write(b'WWTS' + struct.pack('<II', 1, len(c.records)))
        for (fmt, ws, wt, mips, w, h, tf, mf, mnl, mxl, ncol, img, pal, name) in c.records:
            nb = name.encode()
            o.write(struct.pack('<4BHH2B2bHHIII', fmt, ws, wt, mips, w, h, tf, mf, mnl, mxl, ncol, 0, len(img), len(pal), len(nb)))
            o.write(img); o.write(pal); o.write(nb)
    if args.listing:
        lp=args.listing.absolute();lp.parent.mkdir(parents=True,exist_ok=True)
        with lp.open('x',encoding='utf-8')as listing:
            listing.write(''.join('%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s\n' % (r[0], r[4], r[5], r[3], r[1], r[2], r[10], r[13]) for r in c.records))
    tot = sum(len(r[11]) for r in c.records)
    print('unique textures %d (%s), image bytes %.1f MB -> %s' % (len(c.records), c.stats, tot / 1e6, out))


if __name__ == '__main__':
    main()
