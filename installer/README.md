# CitroWaker Installer

A small Windows/Linux program that puts CitroWaker on your New 3DS SD card in three steps:

1. **Choose your disc image**: your own *The Wind Waker* USA disc (`GZLE01`, plain `.iso`, 1.36 GiB).
2. **Choose your SD card**: found automatically when it is plugged in (it looks for the `Nintendo 3DS` folder).
3. **Press Install.**

It copies `CitroWaker.cia` to `/cia/`, your disc image to `/3ds/ww3ds/GZLE01.iso` and, if ticked, builds the
**texture pack** (`/3ds/ww3ds/ww_texpack.bin`, ~290 MB) from your disc image (textures converted on the PC instead of on the console; same picture).
Afterwards install the CIA with FBI on the console and start CitroWaker from the HOME Menu.

| | |
| :--- | :--- |
| Windows 10/11 (64-bit) | `CitroWaker-Installer.exe` |
| Linux x86-64 (glibc 2.35+, e.g. Ubuntu 22.04, Fedora 36+) | `CitroWaker-Installer-linux-x86_64` (`chmod +x` it first) |

Put `CitroWaker.cia` from the same release next to the installer and it is picked up automatically; otherwise use
**Browse** or **Download** (fetches it from this repository's newest release, only when you press the button).

## What it checks and what it never does

- Rejects the European/Japanese discs, later revisions, trimmed (NKit) and compressed (RVZ/WIA/CISO/GCZ) images,
  with the fix spelled out (Dolphin: right-click the game > *Convert File...* > *ISO*).
- Checks the card is a 3DS card, writable, and has room before writing anything.
- Every file is written as `name.part`, flushed, then renamed: cancelling or pulling the card never leaves a broken file.
- Never touches your saves (`/3ds/ww3ds/user/`) or anything else on the card. A disc image already on the card is not copied again.
- Ships no game data. The texture pack is made on your PC from your own disc and stays on your card.

## Command line

```
CitroWaker-Installer --iso GZLE01.iso [--sd /path/to/card] [--cia CitroWaker.cia] [--texture-pack]
CitroWaker-Installer --self-test
```

From source (Python 3.10+ with Tk): `python3 installer/citrowaker_installer.py`. The texture pack also needs the converter built
with `python3 tools/texture-pack/build.py`.

## Building the release programs

`bash installer/build_release.sh` builds both programs in containers (rootless podman): the Linux one on Ubuntu 22.04 with
PyInstaller, the Windows one with a static MinGW-w64 build of the texture converter and PyInstaller under Wine. Tests:
`python3 -m unittest discover -s installer` (also part of `python3 tests/run.py`).

Checked for this release: full installs (disc image + CIA + texture pack, 24,290 records, 0 bad) with the Linux program and with
the Windows program under Wine, plus both windows rendered under a virtual display. Not yet run on a physical Windows PC.
