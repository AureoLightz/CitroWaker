# WW3DS CIA: New 3DS 124 MiB mode

Run from the host with `bash ~/ww-port/n3ds/package_cia.sh`.
It packages the existing ww3ds.elf, without building or relinking the game.
Output: ~/ww-port/n3ds/cia/ww3ds.cia.

## Result

The CIA installed successfully in Azahar 2126.2 (HEAD-219d743a1), booted, started
NDSP audio, and reached frame 360 with the PLAY scene executing on Outset
(sea room 44). The existing env.txt already selects that stage, so this test
preserved it and did not test the ordinary title-screen route.

| Startup value | Preceding 3DSX run | CIA run |
|---|---:|---:|
| New 3DS detected | 1 | 1 |
| Reported application region, KiB | 98,304 | 126,976 |
| Normal heap, KiB | 95,296 | 93,264 |
| Linear heap, KiB | 18,432 | 18,432 |

126,976 KiB is exactly 124 MiB. The requested mode works, but this did NOT
increase usable heap in this Azahar setup. The heap is 2,032 KiB smaller. A second locked test repackaged the exact same
ELF snapshot as a separate 3DSX and reproduced the 95,296 KiB heap, ruling
out differences between Claude's successive builds.
Do not describe this as a 28 MiB heap improvement.

Azahar's 3DSX loader attaches the default APPLICATION resource limit. On New
3DS, its actual region is already 124 MiB while legacy compatibility can
report 96 MiB through the config-memory page. libctru sizes heaps from the
resource-limit SVC, not that compatibility-reported figure. This also explains
why the 3DSX normal and linear heaps together exceed the logged 96 MiB, and
why its logged region-free figure wraps to a huge value.

The CIA reserves 2 MiB of main-thread stack in its exheader to match the
__stacksize__ declared in main_3ds.cpp. Azahar's raw 3DSX loader instead uses
DEFAULT_STACK_SIZE=0x4000 (16 KiB). The extra 2,032 KiB reserved for the CIA
stack matches the measured heap reduction exactly. Do not shrink the CIA
stack merely to make its heap number look larger.

The linked libctru is version 2.7.0. Its weak __ctru_heap_size defaults to zero,
so startup calculates remaining = resource-limit maximum commit minus current
commit, rounded down to a page. Since this program fixes linear heap at 18 MiB,
normal heap receives remaining minus 18 MiB. Code, BSS, stack, and other startup
commitments are therefore deducted before the normal heap is allocated.
A free-application-region value of zero after startup is expected when the
allocator claims the remaining pool; it is not the same as free bytes inside malloc.
No heap-sizing source changes were made.

A real increase would require another memory plan, such as a separately tested
178 MiB mode with allocator/address-layout review. That is not this package;
the requested final RSF stays at 124 MiB. Real-hardware behavior is untested.

## RSF fields

- AccessControlInfo.SystemMode: 64MB -- old-system fallback field.
- AccessControlInfo.SystemModeExt: 124MB -- New 3DS memory mode, encoded by
  makerom as sysmode_ext_124MB (value 1). This is the field that matters here.
- MemoryType: Application; ResourceLimitCategory defaults to application.
- CpuSpeed: 804MHz and EnableL2Cache: true -- stock New 3DS capabilities,
  consistent with the existing application's speed-up request.
- SystemControlInfo.StackSize: 0x200000 -- preserve the declared 2 MiB stack.
- Option.UseOnSD: true; EnableCrypt: false -- SD-installable, decrypted CIA.
- TitleInfo.UniqueId: 0xFF2D7; Category: Application. Title ID:
  000400000FF2D700; product code CTR-P-WW3D. Chosen in the homebrew/prototype
  0xFF000-0xFF3FF block, and not previously installed in this emulator. This
  is not a globally registered guarantee against other homebrew using the ID.
- DirectSdmc filesystem permission and the existing libctru service/mapping
  requirements, including fs:USER, APT:U, gsp::Gpu, dsp::DSP, and ptm:sysm.

Do not add a makerom -desc preset: the script supplies the full RSF access
control information and then verifies the produced New 3DS mode with CTRTool.

## Files and tools

- ww3ds.snapshot.elf and snapshot.json record a stable copied ELF and its SHA256.
  The original ELF/3DSX and object directories are never written.
- ww3ds.rsf is the packaging specification.
- icon.png / banner.png are simple original sail/sea artwork; silence.wav is
  generated silence. icon.smdh and banner.bnr are built from these files.
- No ISO, material bundles, saves, or any SD-card game data are included.
- tools/makerom: Project_CTR makerom-v0.19.0.
- tools/ctrtool: Project_CTR ctrtool-v1.3.0.
- tools/bannertool: Epicpkmn11 release v1.2.2 (binary identifies as v1.2.0).
- tools/bootstrap.py downloads these pinned Linux x86_64 archives and checks
  their SHA256 hashes. Run `python3 cia/tools/bootstrap.py` if tools are missing.
  The existing localhost/ww3ds-clang image is not modified.

The script uses a packaging lock, retries a concurrent ELF rewrite, creates the
CIA under a temporary name, verifies its exheader, and replaces only its own CIA.

## Install and test

File > Install CIA in Azahar, or `flatpak run org.azahar_emu.Azahar --install
/path/to/ww3ds.cia`. The CLI invocation installs and exits. Launch the installed
entry, not the CIA container. The exact installed .app path is in installed-path.txt.
The game still reads sdmc:/3ds/ww3ds/; existing player files stay there.
The CIA has its own title ID and emulator shader/config cache identity; its
user/ saves remain the SD-backed GameCube saves used by the existing program.

Only run tests after taking /tmp/ww3ds-azahar.lock and hold it until every owned
emulator process closes. The supplied test_azahar_locked.sh does this, waits
for the other agent, installs, runs up to frame 360 or 300 seconds, stops only
its own process group, and releases the lock. It never edits env.txt, shots.txt,
or material bundles. The game itself retains its normal runtime miss-cache
and save behavior. Tests may update the normal game log and emulator caches.

Evidence: before-3dsx.log, after-cia.log, inspection.txt, install.stdout.log,
protected-before.json and protected-after.json, plus same-elf-3dsx.log for the
controlled snapshot comparison. The protected test-file hashes
matched. No other agent's process was stopped.

## Primary references

- Makerom memory-mode parsing:
  https://github.com/3DSGuy/Project_CTR/blob/makerom-v0.19.0/makerom/src/exheader.c
- Full RSF template used as the starting point:
  https://gist.github.com/jakcron/9f9f02ffd94d98a72632
- libctru 2.7.0 allocator:
  https://github.com/devkitPro/libctru/blob/v2.7.0/libctru/source/system/allocateHeaps.c
- Installed Azahar revision's memory reporting and region sizes:
  https://github.com/azahar-emu/azahar/blob/219d743a1/src/core/hle/kernel/memory.cpp
- Azahar 3DSX resource/stack setup:
  https://github.com/azahar-emu/azahar/blob/219d743a1/src/core/loader/3dsx.cpp
- Azahar 16 KiB default stack:
  https://github.com/azahar-emu/azahar/blob/219d743a1/src/core/hle/kernel/object.h

## Changes made

New ~/ww-port/n3ds/package_cia.sh and packaging/tooling/evidence files under
~/ww-port/n3ds/cia/. Azahar's title installation was added under its SD title
store; normal logs/caches were produced during the test. No build/relink command
was run, no shared game sources or compiler image were modified, and no original
ww3ds.elf/ww3ds.3dsx/obj/obj3ds files were written.

## Current presentation update

Player delivery remains `cia/ww3ds.cia`, now with a textured CGFX title/animated solid Triforce, matching icon and six-second title-music highlight. The dark neutral bottom controls panel is embedded in the executable. See ../presentation/README.md for source assets, generation and verification. HOME Menu appearance still needs console testing. Existing memory-mode/stack observations above remain unchanged.
