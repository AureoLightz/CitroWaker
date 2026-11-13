import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parent
BINARY=Path(os.environ.get('CITROWAKER_MKTEXPACK',ROOT/'build'/('mktexpack.exe'if os.name=='nt'else'mktexpack')))
class ConverterTests(unittest.TestCase):
    @unittest.skipUnless(BINARY.is_file(),'Build mktexpack first to run native converter tests')
    def test_synthetic_formats_verified_and_deterministic(self):
        records=[]
        for fmt,img in [(0,bytes([0xf0])*32),(1,bytes(range(64))),(6,bytes([255])*256)]:
            name=b'synthetic';records.append(struct.pack('<4BHH2B2bHHIII',fmt,0,0,1,8,8,0,0,0,0,0,0,len(img),0,len(name))+img+name)
        with tempfile.TemporaryDirectory(prefix='citrowaker-tex-')as folder:
            root=Path(folder);source=root/'input.bin';source.write_bytes(b'WWTS'+struct.pack('<II',1,len(records))+b''.join(records));files=[]
            for threads in [1,2]:
                output=root/f'output-{threads}.bin'
                r=subprocess.run([str(BINARY),str(source),str(output),'--threads',str(threads),'--verify'],capture_output=True,text=True)
                self.assertEqual(r.returncode,0,r.stdout+r.stderr);self.assertIn('0 bad',r.stderr)
                data=output.read_bytes();self.assertEqual(data[:4],b'WWTP');self.assertEqual(struct.unpack_from('<I',data,4)[0],2);self.assertGreater(struct.unpack_from('<I',data,8)[0],0);files.append(data)
            self.assertEqual(*files)
            saved=(root/'output-1.bin').read_bytes()
            result=subprocess.run([str(BINARY),str(source),str(root/'output-1.bin')],capture_output=True)
            self.assertNotEqual(result.returncode,0);self.assertEqual((root/'output-1.bin').read_bytes(),saved)
            result=subprocess.run([str(BINARY),str(source),str(source)],capture_output=True)
            self.assertNotEqual(result.returncode,0)
            malformed=root/'malformed.bin';malformed.write_bytes(b'WWTS'+struct.pack('<II',1,0xffffffff))
            result=subprocess.run([str(BINARY),str(malformed),str(root/'bad-output.bin')],capture_output=True,timeout=5)
            self.assertNotEqual(result.returncode,0);self.assertFalse((root/'bad-output.bin').exists())
if __name__=='__main__':unittest.main()
