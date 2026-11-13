from pathlib import Path
import tempfile
import unittest
from audit_assets import inspect
class AuditTests(unittest.TestCase):
    def test_known_bytes_detection_and_no_clearance(self):
        with tempfile.TemporaryDirectory()as folder:
            r=Path(folder);headers=r/'headers';headers.mkdir();payload=bytes(range(64))
            (headers/'fixture.h').write_text('static unsigned char fixture[]={'+','.join(hex(x)for x in payload)+'};')
            elf=r/'test.elf';elf.write_bytes(b'\x7fELF'+b'padding'+payload)
            result=inspect(elf,headers);self.assertTrue(result['known_raw_payload_found']);self.assertEqual(result['embedded_matches'][0]['bytes'],64)
            elf.write_bytes(b'\x7fELF'+bytes(100));result=inspect(elf,headers);self.assertFalse(result['known_raw_payload_found']);self.assertIn('no clearance',result['scope'])
    def test_wrong_input(self):
        with tempfile.TemporaryDirectory()as folder:
            r=Path(folder);p=r/'test.cia';p.write_bytes(b'CIA')
            with self.assertRaises(ValueError):inspect(p,r)
if __name__=='__main__':unittest.main()
