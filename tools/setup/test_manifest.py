import json
from pathlib import Path
import tempfile
import unittest
from create_manifest import create
from prepare import PreparationError
class ManifestTests(unittest.TestCase):
 def test_local_manifest_and_preservation(self):
  with tempfile.TemporaryDirectory()as folder:
   r=Path(folder);cia=r/'test.cia';cia.write_bytes(b'local fixture');bundle=r/'ww_bundle.bin';bundle.write_bytes(b'local fixture');out=r/'release.json'
   create(cia,[bundle],out,'synthetic test')
   j=json.loads(out.read_text());self.assertEqual(j['texture_tools'],'bundled');self.assertEqual(j['material_bundles'][0]['name'],'ww_bundle.bin')
   before=out.read_bytes()
   with self.assertRaises(PreparationError):create(cia,[bundle],out)
   self.assertEqual(out.read_bytes(),before)
 def test_misses_and_duplicates_rejected(self):
  with tempfile.TemporaryDirectory()as folder:
   r=Path(folder);cia=r/'test.cia';cia.write_bytes(b'fixture');b=r/'ww_bundle_misses.bin';b.write_bytes(b'fixture')
   with self.assertRaises(PreparationError):create(cia,[b],r/'release.json')
   b=r/'ww_bundle.bin';b.write_bytes(b'fixture')
   with self.assertRaises(PreparationError):create(cia,[b,b],r/'release.json')
if __name__=='__main__':unittest.main()
