"""Host tests for the CitroWaker Installer: disc/SD/CIA checks and the copy step, with small synthetic files."""
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

import citrowaker_installer as ci


def fake_iso(path: Path, game_id=b"GZLE01", rev=0, size=4096):
    head = bytearray(0x440)
    head[:6] = game_id
    head[7] = rev
    head[0x1C:0x20] = bytes.fromhex("c2339f3d")
    path.write_bytes(bytes(head) + b"\0" * (size - len(head)))


def fake_cia(path: Path):
    path.write_bytes((0x2020).to_bytes(4, "little") + b"\0" * (300 * 1024))


class Checks(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_iso(self):
        iso = self.d / "a.iso"
        with mock.patch.object(ci, "ISO_SIZE", 4096):
            fake_iso(iso)
            self.assertIn("GZLE01", ci.check_iso(iso))
            fake_iso(iso, b"GZLP01")
            with self.assertRaisesRegex(ci.Problem, "European"):
                ci.check_iso(iso)
            fake_iso(iso, rev=2)
            with self.assertRaisesRegex(ci.Problem, "revision"):
                ci.check_iso(iso)
            fake_iso(iso, size=8192)
            with self.assertRaisesRegex(ci.Problem, "trimmed"):
                ci.check_iso(iso)
            iso.write_bytes(b"RVZ\x01" + b"\0" * 2000)
            with self.assertRaisesRegex(ci.Problem, "Convert"):
                ci.check_iso(iso)

    def test_sd_and_cia(self):
        with self.assertRaisesRegex(ci.Problem, "Nintendo 3DS"):
            ci.check_sd(self.d)
        (self.d / "Nintendo 3DS").mkdir()
        self.assertIn("3DS SD card", ci.check_sd(self.d))
        cia = self.d / "CitroWaker.cia"
        fake_cia(cia)
        self.assertIn("CitroWaker.cia", ci.check_cia(cia))
        cia.write_bytes(b"nope")
        with self.assertRaises(ci.Problem):
            ci.check_cia(cia)

    def test_install_keeps_saves_and_skips_same_iso(self):
        sd = self.d / "sd"
        (sd / "Nintendo 3DS").mkdir(parents=True)
        save = sd / "3ds/ww3ds/user/save.gci"
        save.parent.mkdir(parents=True)
        save.write_bytes(b"precious")
        iso, cia = self.d / "g.iso", self.d / "CitroWaker.cia"
        log = []
        with mock.patch.object(ci, "ISO_SIZE", 4096):
            fake_iso(iso)
            fake_cia(cia)
            ci.install(iso, sd, cia, False, log.append, lambda *a: None, threading.Event())
            self.assertEqual((sd / "3ds/ww3ds/GZLE01.iso").read_bytes(), iso.read_bytes())
            self.assertTrue((sd / "cia/CitroWaker.cia").is_file())
            ci.install(iso, sd, cia, False, log.append, lambda *a: None, threading.Event())
        self.assertTrue(any("not copied again" in l for l in log))
        self.assertEqual(save.read_bytes(), b"precious")
        self.assertFalse(list(sd.rglob("*.part")))

    def test_cancel_leaves_no_partial_file(self):
        src, dst = self.d / "src.bin", self.d / "dst.bin"
        src.write_bytes(b"x" * (9 * ci.MiB))
        stop = threading.Event()
        stop.set()
        with self.assertRaises(ci.Cancelled):
            ci.copy_with_progress(src, dst, lambda *a: None, stop, "t")
        self.assertFalse(dst.exists())
        self.assertFalse((self.d / "dst.bin.part").exists())


if __name__ == "__main__":
    unittest.main()
