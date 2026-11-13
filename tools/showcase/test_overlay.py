import tempfile
from pathlib import Path
import unittest
from overlay import read_timings, export_log, make_ass, safe_text

class OverlayTests(unittest.TestCase):
    def test_validation(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'times.csv'
            for body in ['0,nan','-1,20','0,0','0,20\n0,40','1,30\n0,40']:
                p.write_text('time_s,frame_ms\n'+body+'\n')
                with self.assertRaises(ValueError):read_timings(p)
            p.write_text('time_s,frame_ms\n0,20\n0.1,40\n')
            self.assertEqual(read_timings(p),[(0,20),(.1,40)])
    def test_log_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'log';out=Path(d)/'out.csv'
            p.write_text('[frame] 1 draws 5 present 20.0 tick 100\n[frame] 2 draws 5 present 40.0 tick 268123580\n')
            self.assertEqual(export_log(p,out),2)
            self.assertEqual(read_timings(out),[(0,20),(1,40)])
            with self.assertRaises(FileExistsError):export_log(p,out)
            p.write_text('[3ds] frame 60: present 34 ms\n')
            with self.assertRaises(ValueError):export_log(p,Path(d)/'bad.csv')
    def test_discontinuous_log(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'log';p.write_text('[frame] 1 draws 5 present 20.0 tick 100\n[frame] 3 draws 5 present 40.0 tick 200\n')
            with self.assertRaises(ValueError):export_log(p,Path(d)/'out.csv')
    def test_metric_and_stale_values(self):
        text=make_ass([(0,20),(.1,40)],800,480,2,'unit fixture','virtual')
        self.assertIn('VIRTUAL timing',text)
        self.assertIn('33.3 FPS',text)
        self.assertIn('No timing sample',text)
        self.assertEqual(safe_text('{bad}\\N\n'),'(bad)/N ')

if __name__=='__main__':unittest.main()
