import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec=importlib.util.spec_from_file_location('runtime_prepare',Path(__file__).with_name('prepare.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

class PreparationTests(unittest.TestCase):
 def fixture(self,root):
  import json
  specs=json.loads(Path(__file__).with_name('payloads.json').read_text())
  texts={}
  for row in specs:
   row['bytes']=8;row['sha256']=hashlib.sha256(bytes(range(8))).hexdigest()
   values=[int.from_bytes(bytes(range(8))[i:i+row['width']],'big')for i in range(0,8,row['width'])]
   decl=f"static {'u32' if row['width']==4 else 'u8'} {row['symbol']}[] = {{"+','.join(hex(v)for v in values)+'};\n'
   if row['symbol']=='JUTResFONT_Ascfont_fix12':
    opening=decl.split('{',1)[0]+'{\n'
    decl='#if TARGET_PC\n'+opening+'#else\n'+opening+'#endif\n'+decl.split('{',1)[1]
   texts[row['source']]=texts.get(row['source'],'// unrelated current edits must survive\n')+decl
  texts['game/include/JSystem/JUtility/JUTFontData_Ascfont_fix12.h']='extern const u8 JUTResFONT_Ascfont_fix12[];\n'
  texts['game/include/JSystem/JUtility/JUTDirectPrint.h']='class JUTDirectPrint {\nprivate:\n    static u8 sAsciiTable[128];\n};\n'
  texts['build/assets/GZLE01/include/assets/cos_asset_table.inc']='COS_ASSET_ROW("base")\n'*163
  for name,text in texts.items():
   p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
  return specs,texts
 def test_plan_is_readonly_preserves_unrelated_edits(self):
  with tempfile.TemporaryDirectory()as d:
   root=Path(d);specs,old=self.fixture(root);new=m.transform(root,specs)
   for name,text in old.items():self.assertEqual((root/name).read_text(),text)
   for row in specs:
    self.assertIn('unrelated current edits must survive',new[row['source']])
    self.assertIn(f"public_asset_{row['id']}_ready",new[row['source']])
   table=new['build/assets/GZLE01/include/assets/cos_asset_table.inc']
   self.assertEqual(sum(x.startswith('COS_ASSET_ROW')for x in table.splitlines()),174)
 def test_wrong_payload_and_ids_refused_before_writing(self):
  with tempfile.TemporaryDirectory()as d:
   root=Path(d);specs,old=self.fixture(root);specs[-1]['sha256']='0'*64
   with self.assertRaises(ValueError):m.transform(root,specs)
   for name,text in old.items():self.assertEqual((root/name).read_text(),text)
 def test_reapplying_refused(self):
  with tempfile.TemporaryDirectory()as d:
   root=Path(d);specs,old=self.fixture(root)
   for name,text in m.transform(root,specs).items():(root/name).write_text(text)
   with self.assertRaises(ValueError):m.transform(root,specs)

if __name__=='__main__':unittest.main()
