import importlib.util
import struct
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('extract_textures',ROOT/'extract_textures.py')
extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)

class ExtractTests(unittest.TestCase):
    def image(self):
        header=bytearray(32);header[0]=0
        struct.pack_into('>HH',header,2,8,8);header[0x18]=1
        struct.pack_into('>I',header,0x1c,32)
        return bytes(header)+bytes([0xf0])*32
    def test_bti_collect_deduplicate_and_bad_header(self):
        c=extract.Collector();b=self.image()
        c.content('fixture.bti',b);c.content('duplicate.bti',b)
        self.assertEqual(len(c.records),1);self.assertEqual(c.stats['dup'],1)
        c.content('broken.bti',b[:10]);self.assertEqual(c.stats['bad'],1)
    def test_j3d_embedded_header(self):
        image=self.image();head=b'J3D2bmd3'+bytes(24)
        chunk=bytearray(b'TEX1'+bytes(28));struct.pack_into('>I',chunk,4,32+len(image));struct.pack_into('>H',chunk,8,1);struct.pack_into('>I',chunk,12,32)
        c=extract.Collector();c.content('fixture.bmd',head+chunk+image)
        self.assertEqual(len(c.records),1);self.assertEqual(c.stats['j3d'],1)
    def test_yaz0_uncompressed_literals(self):
        raw=self.image();payload=b''.join(b'\xff'+raw[i:i+8]for i in range(0,len(raw),8))
        b=b'Yaz0'+struct.pack('>I',len(raw))+bytes(8)+payload
        c=extract.Collector();c.content('fixture.bti',b)
        self.assertEqual(len(c.records),1)


class ExtractCliTests(unittest.TestCase):
    def test_real_cli_with_synthetic_disc_and_preservation(self):
        import subprocess,sys,tempfile
        with tempfile.TemporaryDirectory()as folder:
            root=Path(folder);disc=root/'fixture.iso';out=root/'textures.bin'
            image=ExtractTests().image();header=bytearray(0x440);header[:6]=b'GZLE01';header[0x1c:0x20]=bytes.fromhex('c2339f3d')
            fst=struct.pack('>6I',0x01000000,0,2,1,0x480,len(image))+b'\0fixture.bti\0'
            struct.pack_into('>II',header,0x424,0x440,len(fst));disc.write_bytes(header+fst+bytes(0x480-0x440-len(fst))+image);before=disc.read_bytes()
            cmd=[sys.executable,str(ROOT/'extract_textures.py'),str(disc),str(out)]
            result=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(out.read_bytes()[:12],b'WWTS'+struct.pack('<II',1,1));saved=out.read_bytes()
            self.assertNotEqual(subprocess.run(cmd,capture_output=True).returncode,0);self.assertEqual(out.read_bytes(),saved)
            self.assertNotEqual(subprocess.run(cmd[:-1]+[str(disc)],capture_output=True).returncode,0);self.assertEqual(disc.read_bytes(),before)

if __name__=='__main__':unittest.main()
