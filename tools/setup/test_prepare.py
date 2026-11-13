import json,struct,tempfile,threading,unittest
from pathlib import Path
from unittest.mock import patch
import prepare
class ExportTests(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup);self.root=Path(self.tmp.name)
  h=bytearray(0x440);h[:6]=b'GZLE01';h[0x1c:0x20]=bytes.fromhex('c2339f3d');(self.root/'game.iso').write_bytes(h)
  (self.root/'game.cia').write_bytes(b'\x20\x20\0\0'+bytes(100))
  for name in ['extract.py','parser.py','converter','bundle.bin']:(self.root/name).write_bytes(b'tool fixture')
  self.config={'schema':1,'cia':'game.cia','extractor':'extract.py','disc_parser':'parser.py','texture_converter':'converter','material_bundles':[{'source':'bundle.bin','name':'ww_bundle.bin'}]};self.manifest=self.root/'release.json';self.save()
 def save(self):self.manifest.write_text(json.dumps(self.config))
 def tools(self,args,cancel,log):
  if '--extract-worker' in args:Path(args[-1]).write_bytes(b'WWTS')
  else:Path(args[2]).write_bytes(struct.pack('<4sIII',b'WWTP',2,1,123))
 def export(self,cancel=None):
  with patch.object(prepare,'run_tool',side_effect=self.tools):return prepare.prepare(self.root/'game.iso',self.manifest,self.root/'output',cancel=cancel,log=lambda s:None)
 def test_export_layout_hashes_and_no_testing_files(self):
  r=self.export();out=self.root/'output';self.assertEqual(len(r['sha256']),4)
  self.assertTrue((out/'3ds/ww3ds/user').is_dir());self.assertTrue((out/'cia/CitroWaker.cia').is_file())
  self.assertFalse((out/'texsrc.bin').exists());self.assertFalse((out/'3ds/ww3ds/env.txt').exists())
  self.assertEqual((out/'3ds/ww3ds/GZLE01.iso').read_bytes(),(self.root/'game.iso').read_bytes())
 def test_existing_output_is_untouched(self):
  out=self.root/'output';out.mkdir();(out/'save').write_text('preserve')
  with self.assertRaises(prepare.PreparationError):self.export()
  self.assertEqual((out/'save').read_text(),'preserve')
 def test_bad_iso(self):
  (self.root/'game.iso').write_bytes(bytes(0x440))
  with self.assertRaises(prepare.PreparationError):self.export()
 def test_unsafe_and_duplicate_names(self):
  for name in ['../escape.bin','ww_bundle/escape.bin','ww_bundle_misses.bin','ww_bundle:stream.bin','ww_bundle?.bin','ww_bundle .bin']:
   self.config['material_bundles']=[{'source':'bundle.bin','name':name}];self.save()
   with self.assertRaises(prepare.PreparationError):self.export()
  self.config['material_bundles']=[{'source':'bundle.bin','name':'ww_bundle.bin'}]*2;self.save()
  with self.assertRaises(prepare.PreparationError):self.export()
 def test_cancel_and_failure_leave_no_export(self):
  cancel=threading.Event();cancel.set()
  with self.assertRaises(prepare.Cancelled):self.export(cancel)
  self.assertFalse((self.root/'output').exists());self.assertFalse(list(self.root.glob('.citrowaker-*')))
  with patch.object(prepare,'run_tool',side_effect=prepare.PreparationError('converter failed')):
   with self.assertRaises(prepare.PreparationError):prepare.prepare(self.root/'game.iso',self.manifest,self.root/'output',log=lambda s:None)
  self.assertFalse((self.root/'output').exists());self.assertFalse(list(self.root.glob('.citrowaker-*')))
if __name__=='__main__':unittest.main()
