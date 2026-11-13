import tempfile
from pathlib import Path
import unittest
from report import parse, metrics, summary, compare

class Reports(unittest.TestCase):
    def test_weighted_blocks_and_sparse_samples(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'log.txt';p.write_text('[present] frame 60: 60 intervals mean 10.0 ms (100 fps) min 2 max 15 fence 0\n[present] frame 120: 30 intervals mean 40.0 ms (25 fps) min 30 max 100 fence 0\n[3ds] frame 120: 2/3 draws (gx 4) process 40 ms render 20 ms (build 5 plan 2 draw 3) present 50 ms\n[mem] host heap: in use 123 KB, taken from the system 200 KB of 300 KB (sbrk room 100 KB)\n')
            r=parse(p);m=metrics(r);self.assertEqual(m['block-weighted frame ms'],20);self.assertEqual(m['block-weighted FPS'],50);self.assertEqual(m['sampled render median ms'],20)
            self.assertIn('cannot infer',summary(r));self.assertEqual(len(parse(p,100)['present']),1)
    def test_missing_and_probe_tolerance(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'log';p.write_text('[depth-probe] test Z scale 1 offset 0: centre 7fffff min 7fffff max 7fffff stencil 00 expect 800000\n[capture] saved frame 123\n')
            r=parse(p);self.assertIsNone(metrics(r)['block-weighted FPS']);self.assertIn('±1 1',summary(r));self.assertIn('not logged',compare(r,r));self.assertTrue(r['warnings'])

if __name__=='__main__':unittest.main()
