#!/usr/bin/env python3
"""CitroWaker Installer: puts CitroWaker and your own Wind Waker disc image on a New 3DS SD card.

Windows and Linux. A window opens when started without arguments; `--help` lists the command-line mode.
Nothing is downloaded or uploaded unless you press "Download"; your saves (/3ds/ww3ds/user) are never touched.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import queue
import shutil
import string
import subprocess
import sys
import tempfile
import threading
import time
import ssl
import urllib.error
import urllib.request
from pathlib import Path

APP = "CitroWaker Installer"
VERSION = "1.0.0"
REPO = "AureoLightz/CitroWaker"
ISO_SIZE = 1_459_978_240            # every full GameCube disc image
GAME_DIR = Path("3ds") / "ww3ds"    # the port's data folder on the SD card
CIA_NAME = "CitroWaker.cia"
PACK_NAME = "ww_texpack.bin"
MiB = 1 << 20


# ----------------------------------------------------------------------------------------------- resources
def resource_dir() -> Path:
    """Folder holding bundled tools (PyInstaller's unpack folder when frozen, else this file's folder)."""
    if getattr(sys, "frozen", False):
        return Path(getattr(sys, "_MEIPASS", Path(sys.executable).parent))
    return Path(__file__).resolve().parent


def app_dir() -> Path:
    """Folder the installer itself sits in (the release zip puts CitroWaker.cia here)."""
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent


def texture_tool_dir() -> Path | None:
    for d in (resource_dir() / "texture-pack", Path(__file__).resolve().parent.parent / "tools" / "texture-pack"):
        if (d / "extract_textures.py").is_file():
            return d
    return None


def converter_path() -> Path | None:
    exe = "mktexpack.exe" if os.name == "nt" else "mktexpack"
    for p in (resource_dir() / exe, resource_dir() / "texture-pack" / "build" / exe,
              Path(__file__).resolve().parent.parent / "tools" / "texture-pack" / "build" / exe):
        if p.is_file():
            return p
    return None


# ----------------------------------------------------------------------------------------------- checks
class Problem(Exception):
    """A message for the player: what is wrong and what to do about it."""


def check_iso(path: Path) -> str:
    """Returns a description of a usable disc image or raises Problem with advice."""
    if not path.is_file():
        raise Problem("That file does not exist.")
    size = path.stat().st_size
    with path.open("rb") as f:
        head = f.read(0x440)
    if head[:4] == b"RVZ\x01" or head[:4] == b"WIA\x01":
        raise Problem("This is a compressed Dolphin image (RVZ/WIA). In Dolphin, right-click the game, choose "
                      "'Convert File...', pick the format 'ISO' and use the new file.")
    if head[:4] == b"CISO" or head[:4] == b"GCZ\x01" or head[:4] == b"\x01\xc0\x0b\xb1":
        raise Problem("This is a compressed image (CISO/GCZ). Convert it to a plain ISO with Dolphin "
                      "('Convert File...', format 'ISO').")
    if len(head) < 0x440 or head[0x1C:0x20] != bytes.fromhex("c2339f3d"):
        raise Problem("This is not a GameCube disc image. Choose your Wind Waker .iso file.")
    game_id = head[:6].decode("ascii", "replace")
    if game_id != "GZLE01":
        region = {"GZLP01": "European", "GZLJ01": "Japanese"}.get(game_id, "a different game or region")
        raise Problem(f"This disc is {region} ({game_id}). CitroWaker needs the USA version (GZLE01).")
    if head[7] != 0:
        raise Problem("This is a later revision of the USA disc. CitroWaker needs the original release (revision 0).")
    if size != ISO_SIZE:
        raise Problem(f"The image is {size:,} bytes, a full disc is {ISO_SIZE:,}. It may be trimmed (NKit) or "
                      "damaged; make a full ISO with Dolphin or from your disc again.")
    return "The Legend of Zelda: The Wind Waker (USA, GZLE01)"


def check_cia(path: Path) -> str:
    if not path.is_file():
        raise Problem("The CitroWaker .cia file was not found.")
    size = path.stat().st_size
    with path.open("rb") as f:
        head = f.read(0x20)
    if len(head) < 0x20 or int.from_bytes(head[:4], "little") != 0x2020 or size < 256 * 1024:
        raise Problem("This does not look like a 3DS .cia file. Use CitroWaker.cia from the CitroWaker release.")
    return f"{path.name} ({size / MiB:.1f} MB)"


def free_bytes(path: Path) -> int:
    try:
        return shutil.disk_usage(path).free
    except OSError:
        return 0


def check_sd(root: Path) -> str:
    if not root.is_dir():
        raise Problem("That folder does not exist. Choose the SD card itself (its top folder).")
    if not os.access(root, os.W_OK):
        raise Problem("This drive cannot be written to. Is the SD card's lock switch in the 'unlocked' position?")
    if not (root / "Nintendo 3DS").is_dir():
        raise Problem("This is not a 3DS SD card (there is no 'Nintendo 3DS' folder). Choose the card's top folder, "
                      "not a folder inside it.")
    return f"3DS SD card, {free_bytes(root) / (1 << 30):.1f} GB free"


def find_sd_cards() -> list[Path]:
    """Mounted drives that look like a 3DS SD card (have a 'Nintendo 3DS' folder)."""
    found: list[Path] = []
    if os.name == "nt":
        import ctypes
        mask = ctypes.windll.kernel32.GetLogicalDrives()
        for i, letter in enumerate(string.ascii_uppercase):
            if mask & (1 << i):
                root = Path(f"{letter}:\\")
                if ctypes.windll.kernel32.GetDriveTypeW(f"{letter}:\\") in (2, 3) and (root / "Nintendo 3DS").is_dir():
                    found.append(root)
    else:
        user = os.environ.get("USER") or ""
        bases = [Path("/run/media") / user, Path("/media") / user, Path("/media"), Path("/mnt"), Path("/Volumes")]
        for base in bases:
            try:
                for d in base.iterdir():
                    if d.is_dir() and (d / "Nintendo 3DS").is_dir() and d not in found:
                        found.append(d)
            except OSError:
                pass
    return found


def find_cia() -> Path | None:
    for d in (app_dir(), app_dir().parent, Path.cwd()):
        p = d / CIA_NAME
        if p.is_file():
            return p
    return None


_CA_FILES = (
    "/etc/pki/tls/certs/ca-bundle.crt",       # Fedora, Bazzite, SteamOS-like, RHEL
    "/etc/ssl/certs/ca-certificates.crt",     # Debian, Ubuntu, Arch, Mint
    "/etc/ssl/ca-bundle.pem",                 # openSUSE
    "/etc/ssl/cert.pem",                      # Alpine, macOS, BSD
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
)


def _ssl_context() -> ssl.SSLContext:
    """Certificate roots for the download. The one-file Linux build carries an OpenSSL that looks where the build
    system kept its CA bundle, which other distributions do not have: load the system bundle from where this
    distribution keeps it, else the certifi bundle packed into the installer."""
    ctx = ssl.create_default_context()
    if ctx.cert_store_stats().get("x509_ca", 0):
        return ctx
    for f in [os.environ.get("SSL_CERT_FILE", "")] + list(_CA_FILES):
        if f and os.path.isfile(f):
            try:
                ctx.load_verify_locations(cafile=f)
                if ctx.cert_store_stats().get("x509_ca", 0):
                    return ctx
            except (ssl.SSLError, OSError):
                pass
    try:
        import certifi  # packed by build_release.sh
        ctx.load_verify_locations(cafile=certifi.where())
    except Exception:
        pass
    return ctx


def _open(req, timeout):
    return urllib.request.urlopen(req, timeout=timeout, context=_ssl_context())


def download_latest_cia(dest_dir: Path, log) -> Path:
    """Fetches CitroWaker.cia from the newest GitHub release (only when the player asks for it)."""
    url = f"https://api.github.com/repos/{REPO}/releases/latest"
    req = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json", "User-Agent": "CitroWaker-Installer"})
    try:
        with _open(req, 30) as r:
            info = json.load(r)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            raise Problem("No CitroWaker release is published yet. Put CitroWaker.cia next to the installer and try again.")
        raise
    asset = next((a for a in info.get("assets", []) if a.get("name") == CIA_NAME), None)
    if not asset:
        raise Problem("The latest release has no CitroWaker.cia. Download it from the releases page by hand.")
    dest = dest_dir / CIA_NAME
    log(f"Downloading {CIA_NAME} ({info.get('tag_name', 'latest')})...")
    req = urllib.request.Request(asset["browser_download_url"], headers={"User-Agent": "CitroWaker-Installer"})
    with _open(req, 60) as r, open(dest.with_suffix(".part"), "wb") as f:
        shutil.copyfileobj(r, f)
    os.replace(dest.with_suffix(".part"), dest)
    return dest


# ----------------------------------------------------------------------------------------------- install
class Cancelled(Exception):
    pass


def same_file(a: Path, b: Path) -> bool:
    """Size plus the hash of the first and last MiB: enough to skip re-copying the same 1.4 GB image."""
    try:
        if a.stat().st_size != b.stat().st_size:
            return False
        def sig(p: Path) -> bytes:
            h = hashlib.sha256()
            with p.open("rb") as f:
                h.update(f.read(MiB))
                f.seek(max(0, p.stat().st_size - MiB))
                h.update(f.read(MiB))
            return h.digest()
        return sig(a) == sig(b)
    except OSError:
        return False


def copy_with_progress(src: Path, dst: Path, progress, cancel: threading.Event, label: str):
    """Copies to dst.part, flushes it to the card, then renames: a pulled card never leaves a half file under the real name."""
    total = src.stat().st_size
    part = dst.with_name(dst.name + ".part")
    done = 0
    try:
        with src.open("rb") as fi, part.open("wb") as fo:
            while True:
                if cancel.is_set():
                    raise Cancelled()
                chunk = fi.read(4 * MiB)
                if not chunk:
                    break
                fo.write(chunk)
                done += len(chunk)
                progress(label, done / total if total else 1.0)
            fo.flush()
            os.fsync(fo.fileno())
        os.replace(part, dst)
    except BaseException:
        try:
            part.unlink()
        except OSError:
            pass
        raise


def build_texture_pack(iso: Path, out: Path, log, progress, cancel: threading.Event, threads: int = 2):
    tools = texture_tool_dir()
    conv = converter_path()
    if not tools or not conv:
        raise Problem("The texture tools are missing from this installer; untick 'Build the texture pack' or use the full release.")
    with tempfile.TemporaryDirectory(prefix="citrowaker-") as tmp:
        src = Path(tmp) / "texsrc.bin"
        pack = Path(tmp) / PACK_NAME
        progress("Reading textures from the disc image", 0.0)
        log("Reading textures from the disc image (1-3 minutes)...")
        if getattr(sys, "frozen", False):
            cmd = [sys.executable, "--extract-textures", str(iso), str(src)]
        else:
            cmd = [sys.executable, str(Path(__file__).resolve()), "--extract-textures", str(iso), str(src)]
        run_child(cmd, log, cancel)
        if cancel.is_set():
            raise Cancelled()
        progress("Converting textures for the 3DS GPU", 0.4)
        log("Converting textures for the 3DS GPU (a few minutes)...")
        run_child([str(conv), str(src), str(pack), "--threads", str(threads), "--verify"], log, cancel)
        progress("Copying the texture pack", 0.9)
        copy_with_progress(pack, out, lambda l, f: progress("Copying the texture pack", 0.9 + 0.1 * f), cancel, "texture pack")


def run_child(cmd: list[str], log, cancel: threading.Event):
    flags = 0x08000000 if os.name == "nt" else 0   # CREATE_NO_WINDOW: no console flashing up on Windows
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, creationflags=flags)
    lines: list[str] = []
    assert proc.stdout
    for line in proc.stdout:
        line = line.rstrip()
        if line:
            lines.append(line)
            log("  " + line)
        if cancel.is_set():
            proc.kill()
            proc.wait()
            raise Cancelled()
    if proc.wait() != 0:
        raise Problem("A texture tool failed:\n" + "\n".join(lines[-12:]))


def install(iso: Path, sd: Path, cia: Path, texture_pack: bool, log, progress, cancel: threading.Event) -> list[str]:
    """Does the whole job; returns the steps the player still has to do on the console."""
    log(f"{APP} {VERSION}")
    log("Disc image: " + check_iso(iso))
    log("SD card: " + check_sd(sd))
    log("CitroWaker: " + check_cia(cia))
    game = sd / GAME_DIR
    iso_dst = game / "GZLE01.iso"
    pack_dst = game / PACK_NAME
    need = cia.stat().st_size + 64 * MiB
    copy_iso = not same_file(iso, iso_dst)
    if copy_iso:
        need += ISO_SIZE - (iso_dst.stat().st_size if iso_dst.exists() else 0)
    if texture_pack and not pack_dst.exists():
        need += 320 * MiB
    if free_bytes(sd) < need:
        raise Problem(f"The SD card needs {need / (1 << 30):.2f} GB free; it has {free_bytes(sd) / (1 << 30):.2f} GB. "
                      "Free some space (or use a bigger card) and try again.")
    game.mkdir(parents=True, exist_ok=True)
    (sd / "cia").mkdir(exist_ok=True)

    progress("Copying CitroWaker", 0.0)
    copy_with_progress(cia, sd / "cia" / CIA_NAME, progress, cancel, "Copying CitroWaker")
    log(f"Copied {CIA_NAME} to /cia/")

    if copy_iso:
        log("Copying the disc image to /3ds/ww3ds/ (1.4 GB, a few minutes on most cards)...")
        copy_with_progress(iso, iso_dst, progress, cancel, "Copying the disc image")
        log("Copied GZLE01.iso")
    else:
        log("The disc image is already on the card: not copied again.")

    if texture_pack:
        if pack_dst.exists():
            old = pack_dst.with_name(PACK_NAME + ".old")
            log("Replacing the existing texture pack (the old one is kept until the new one is in place)...")
            if old.exists():
                old.unlink()
            pack_dst.rename(old)
            try:
                build_texture_pack(iso, pack_dst, log, progress, cancel)
            except BaseException:
                if not pack_dst.exists():
                    old.rename(pack_dst)
                raise
            old.unlink()
        else:
            build_texture_pack(iso, pack_dst, log, progress, cancel)
        log("Texture pack ready.")

    marker = game / "INSTALLED_BY.txt"
    marker.write_text(f"{APP} {VERSION}, {time.strftime('%Y-%m-%d %H:%M')}\n"
                      f"CIA: {cia.name} sha256 {sha256(cia)}\n", encoding="utf-8")
    progress("Done", 1.0)
    return ["Safely remove the SD card and put it back in your New 3DS.",
            "Start FBI on the 3DS, open 'SD' > 'cia', select CitroWaker.cia and choose 'Install CIA'.",
            "Go back to the HOME Menu and start CitroWaker.",
            "Settings: press START + SELECT in game (or touch 'Settings' on the bottom screen)."]


def sha256(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(4 * MiB), b""):
            h.update(chunk)
    return h.hexdigest()


# ----------------------------------------------------------------------------------------------- window
def run_gui():
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    root = tk.Tk()
    root.title(APP)
    root.minsize(640, 520)
    style = ttk.Style(root)
    if "clam" in style.theme_names() and os.name != "nt":
        style.theme_use("clam")
    style.configure("Title.TLabel", font=("TkDefaultFont", 16, "bold"))
    style.configure("Step.TLabel", font=("TkDefaultFont", 11, "bold"))
    style.configure("Ok.TLabel", foreground="#1b7a3a")
    style.configure("Bad.TLabel", foreground="#b3261e")
    style.configure("Big.TButton", font=("TkDefaultFont", 12, "bold"), padding=8)

    iso_var, sd_var, cia_var = tk.StringVar(), tk.StringVar(), tk.StringVar()
    pack_var = tk.BooleanVar(value=converter_path() is not None)
    status = {"iso": False, "sd": False, "cia": False}
    events: queue.Queue = queue.Queue()
    cancel = threading.Event()
    worker: list[threading.Thread] = []

    outer = ttk.Frame(root, padding=18)
    outer.pack(fill="both", expand=True)
    ttk.Label(outer, text="CitroWaker for New Nintendo 3DS", style="Title.TLabel").pack(anchor="w")
    ttk.Label(outer, text="Three steps: choose your disc image, choose your SD card, press Install.",
              wraplength=600).pack(anchor="w", pady=(2, 12))

    def step(number: int, title: str, var: tk.StringVar, browse, extra=None):
        box = ttk.Frame(outer)
        box.pack(fill="x", pady=4)
        ttk.Label(box, text=f"{number}. {title}", style="Step.TLabel").pack(anchor="w")
        row = ttk.Frame(box)
        row.pack(fill="x", pady=2)
        entry = ttk.Entry(row, textvariable=var)
        entry.pack(side="left", fill="x", expand=True)
        if extra:
            extra(row)
        ttk.Button(row, text="Browse...", command=browse).pack(side="left", padx=(6, 0))
        msg = ttk.Label(box, text="", wraplength=600)
        msg.pack(anchor="w")
        return msg

    def pick_iso():
        p = filedialog.askopenfilename(title="Your Wind Waker disc image",
                                       filetypes=[("GameCube disc image", "*.iso *.gcm"), ("All files", "*.*")])
        if p:
            iso_var.set(p)

    def pick_sd():
        p = filedialog.askdirectory(title="Your 3DS SD card (its top folder)")
        if p:
            sd_var.set(p)

    def pick_cia():
        p = filedialog.askopenfilename(title="CitroWaker.cia", filetypes=[("3DS CIA", "*.cia"), ("All files", "*.*")])
        if p:
            cia_var.set(p)

    def detect(_row=None):
        cards = find_sd_cards()
        if cards:
            sd_var.set(str(cards[0]))
        else:
            sd_msg.configure(text="No 3DS SD card found. Insert it, then press 'Find' or use Browse.", style="Bad.TLabel")

    def download():
        if not messagebox.askyesno(APP, "Download the newest CitroWaker.cia from github.com/" + REPO + "?"):
            return
        def job():
            try:
                p = download_latest_cia(Path(tempfile.gettempdir()), log)
                events.put(("cia", str(p)))
            except Exception as e:   # network errors, missing asset
                events.put(("error", f"Download failed: {e}"))
        threading.Thread(target=job, daemon=True).start()

    iso_msg = step(1, "Your Wind Waker disc image (USA, .iso)", iso_var, pick_iso)
    sd_msg = step(2, "Your 3DS SD card", sd_var, pick_sd,
                  extra=lambda row: ttk.Button(row, text="Find", command=detect).pack(side="left", padx=(6, 0)))
    cia_msg = step(3, "CitroWaker game file (CitroWaker.cia)", cia_var, pick_cia,
                   extra=lambda row: ttk.Button(row, text="Download", command=download).pack(side="left", padx=(6, 0)))

    opts = ttk.Frame(outer)
    opts.pack(fill="x", pady=(8, 4))
    pack_box = ttk.Checkbutton(opts, variable=pack_var,
                               text="Also build the texture pack (optional: converts textures on this PC instead of on the 3DS; "
                                    "a few minutes, about 300 MB)")
    pack_box.pack(anchor="w")
    if converter_path() is None:
        pack_box.state(["disabled"])

    progress_text = ttk.Label(outer, text="")
    progress_text.pack(anchor="w", pady=(10, 0))
    bar = ttk.Progressbar(outer, mode="determinate", maximum=1000)
    bar.pack(fill="x", pady=4)

    buttons = ttk.Frame(outer)
    buttons.pack(fill="x", pady=6)
    install_btn = ttk.Button(buttons, text="Install", style="Big.TButton")
    install_btn.pack(side="right")
    cancel_btn = ttk.Button(buttons, text="Cancel", command=lambda: cancel.set())
    cancel_btn.pack(side="right", padx=8)
    cancel_btn.state(["disabled"])

    logbox = tk.Text(outer, height=8, wrap="word", relief="flat", background=root.cget("background"))
    logbox.pack(fill="both", expand=True, pady=(6, 0))
    logbox.configure(state="disabled")

    def log(text: str):
        events.put(("log", text))

    def progress(label: str, fraction: float):
        events.put(("progress", (label, fraction)))

    def show(msg: ttk.Label, fn, value: str, key: str):
        if not value.strip():
            msg.configure(text="", style="TLabel")
            status[key] = False
        else:
            try:
                msg.configure(text="✔ " + fn(Path(value.strip())), style="Ok.TLabel")
                status[key] = True
            except Problem as e:
                msg.configure(text="✖ " + str(e), style="Bad.TLabel")
                status[key] = False
            except OSError as e:
                msg.configure(text="✖ " + str(e), style="Bad.TLabel")
                status[key] = False
        busy = bool(worker and worker[0].is_alive())
        install_btn.state(["!disabled"] if all(status.values()) and not busy else ["disabled"])

    iso_var.trace_add("write", lambda *_: show(iso_msg, check_iso, iso_var.get(), "iso"))
    sd_var.trace_add("write", lambda *_: show(sd_msg, check_sd, sd_var.get(), "sd"))
    cia_var.trace_add("write", lambda *_: show(cia_msg, check_cia, cia_var.get(), "cia"))

    def start():
        cancel.clear()
        for w in (install_btn,):
            w.state(["disabled"])
        cancel_btn.state(["!disabled"])
        def job():
            try:
                steps = install(Path(iso_var.get().strip()), Path(sd_var.get().strip()), Path(cia_var.get().strip()),
                                pack_var.get(), log, progress, cancel)
                events.put(("done", steps))
            except Cancelled:
                events.put(("error", "Cancelled. Nothing half-copied was left on the card."))
            except Problem as e:
                events.put(("error", str(e)))
            except OSError as e:
                events.put(("error", f"Could not write to the SD card: {e}\nCheck the card is still inserted and not full."))
        t = threading.Thread(target=job, daemon=True)
        worker[:] = [t]
        t.start()

    install_btn.configure(command=start)
    install_btn.state(["disabled"])

    def pump():
        try:
            while True:
                kind, value = events.get_nowait()
                if kind == "log":
                    logbox.configure(state="normal")
                    logbox.insert("end", value + "\n")
                    logbox.see("end")
                    logbox.configure(state="disabled")
                elif kind == "progress":
                    label, fraction = value
                    progress_text.configure(text=f"{label}  {fraction * 100:.0f}%")
                    bar["value"] = int(fraction * 1000)
                elif kind == "cia":
                    cia_var.set(value)
                elif kind in ("done", "error"):
                    cancel_btn.state(["disabled"])
                    show(cia_msg, check_cia, cia_var.get(), "cia")
                    if kind == "done":
                        text = "CitroWaker is on your SD card.\n\nNow:\n" + "\n".join(f"{i + 1}. {s}" for i, s in enumerate(value))
                        log("")
                        log(text)
                        messagebox.showinfo(APP, text)
                    else:
                        log(value)
                        messagebox.showerror(APP, value)
        except queue.Empty:
            pass
        root.after(100, pump)

    found = find_cia()
    if found:
        cia_var.set(str(found))
    detect()
    if os.environ.get("CITROWAKER_SCREENSHOT"):
        root.after(1500, lambda: root.destroy())
    pump()
    root.mainloop()


# ----------------------------------------------------------------------------------------------- command line
def run_cli(argv: list[str]) -> int:
    p = argparse.ArgumentParser(prog="citrowaker-installer", description=__doc__)
    p.add_argument("--iso", type=Path, required=True, help="your USA Wind Waker disc image (GZLE01 .iso)")
    p.add_argument("--sd", type=Path, help="the 3DS SD card's top folder (default: the one found)")
    p.add_argument("--cia", type=Path, help="CitroWaker.cia (default: next to the installer)")
    p.add_argument("--texture-pack", action="store_true", help="also build the texture pack")
    a = p.parse_args(argv)
    sd = a.sd or (find_sd_cards() or [None])[0]
    cia = a.cia or find_cia()
    if not sd:
        print("No 3DS SD card found: pass --sd", file=sys.stderr)
        return 2
    if not cia:
        print("CitroWaker.cia not found: pass --cia", file=sys.stderr)
        return 2
    last = [""]
    tty = sys.stdout.isatty()
    def progress(label, fraction):
        text = f"{label} {fraction * 100:.0f}%"
        if tty and text != last[0]:
            last[0] = text
            print("\r" + text.ljust(60), end="", flush=True)
    def log(text):
        print(("\r" + " " * 60 + "\r" if tty else "") + text, flush=True)
    try:
        steps = install(a.iso, sd, cia, a.texture_pack, log, progress, threading.Event())
    except (Problem, OSError) as e:
        print("\nError: " + str(e), file=sys.stderr)
        return 1
    print("\nDone. Now:")
    for i, s in enumerate(steps):
        print(f"  {i + 1}. {s}")
    return 0


def self_test() -> int:
    """For release checks: the bundled pieces are present and run."""
    ok = True
    print(f"{APP} {VERSION} on {sys.platform}, Python {sys.version.split()[0]}")
    try:
        import tkinter  # noqa: F401
        print("tkinter: ok")
    except Exception as e:
        print("tkinter: MISSING", e)
        ok = False
    tools, conv = texture_tool_dir(), converter_path()
    print("texture extractor:", tools or "MISSING")
    print("texture converter:", conv or "MISSING")
    if conv:
        r = subprocess.run([str(conv)], capture_output=True, text=True)
        print("converter runs: " + ("ok" if "usage" in (r.stderr + r.stdout) else "NO"))
        ok &= "usage" in (r.stderr + r.stdout)
    ok &= bool(tools and conv)
    print("SD cards found:", [str(p) for p in find_sd_cards()])
    return 0 if ok else 1


def ensure_stdio():
    """A windowed build has no stdout: attach to the console it was started from (command-line use), else discard."""
    if sys.stdout is not None and sys.stderr is not None:
        return
    if os.name == "nt":
        try:
            import ctypes
            if ctypes.windll.kernel32.AttachConsole(-1):
                stream = open("CONOUT$", "w", buffering=1, encoding="utf-8", errors="replace")
                sys.stdout = sys.stdout or stream
                sys.stderr = sys.stderr or stream
        except Exception:
            pass
    if sys.stdout is None:
        sys.stdout = open(os.devnull, "w")
    if sys.stderr is None:
        sys.stderr = sys.stdout


def main() -> int:
    ensure_stdio()
    if len(sys.argv) > 1 and sys.argv[1] == "--extract-textures":
        tools = texture_tool_dir()
        if not tools:
            print("texture extractor missing", file=sys.stderr)
            return 2
        sys.path.insert(0, str(tools))
        sys.path.insert(0, str(tools / "third_party" / "switchwaker"))
        import extract_textures   # noqa: E402  (bundled tools/texture-pack)
        sys.argv = ["extract_textures.py"] + sys.argv[2:4]
        extract_textures.main()
        return 0
    if len(sys.argv) > 1 and sys.argv[1] == "--self-test":
        return self_test()
    if len(sys.argv) > 1:
        return run_cli(sys.argv[1:])
    run_gui()
    return 0


if __name__ == "__main__":
    sys.exit(main())
