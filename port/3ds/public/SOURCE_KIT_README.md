# CitroWaker: complete corresponding source for `CitroWaker.cia`

This archive is the source of the `CitroWaker.cia` published next to it. That binary links Dolphin's DSP HLE
(`SwitchWaker/native/dsp_hle`, `SwitchWaker/ref/recompcore/Source/Core/Core/HW/DSPHLE`), which is licensed
GPL-2.0-or-later, so the CIA as a whole is distributed under the GNU GPL version 2 or later. This archive is offered
under the same terms. The full licence texts are in `SwitchWaker/native/dsp_hle/LICENSE` and next to each component.

Contents are exactly the files the build of that CIA read, found by tracing a complete build, plus the build scripts:

| Folder | What | Licence |
| :--- | :--- | :--- |
| `SwitchWaker/game/`, `SwitchWaker/native/` | decompiled game code (zeldaret/tww via SwitchWaker) and its PC/SDK layer | see `SwitchWaker/LICENSE*`, `THIRD_PARTY.md` |
| `SwitchWaker/native/dsp_hle/`, `SwitchWaker/ref/recompcore/` | Dolphin DSP HLE and its adapter | GPL-2.0-or-later |
| `SwitchWaker/build/native-linux/_deps/` | Aurora (GX SDK), fmt and other dependency sources the build compiles | their own licences, included |
| `SwitchWaker/build/assets/` | zero-content stub headers and enum metadata. **No game data**: the game reads its data arrays from the player's disc image at start-up | |
| `n3ds/` | 3DS platform layer, build scripts, patched citro3d, PGO profile, CIA packaging | MIT (citro3d: zlib) |
| `hw6/` | GameCube GX to 3DS PICA renderer | MIT |
| `original-home/` | generator for the HOME Menu banner and icon | MIT |

## Building

Requirements: Linux with podman, and the build image described by `n3ds/Containerfile` (a Debian 13 base with clang 19
plus devkitARM from `docker.io/devkitpro/devkitarm`; libctru 2.7.0). Tag it `localhost/ww3ds-clang`.

```sh
cd n3ds
export SOURCE_DATE_EPOCH=$(cat ../SOURCE_DATE_EPOCH)   # the compiler's __DATE__/__TIME__ of the published build
N3DS_PUBLIC=1 N3DS_PGO=use bash build.sh               # -> public-build/ww3ds.elf
ORIGINAL_HOME=$PWD/../original-home bash public/package_public.sh                               # -> public-build/CitroWaker.cia
```

Packaging uses makerom, bannertool and ctrtool in `n3ds/cia/tools/` (from their upstream releases; not included).
A build from this archive reproduces the published `ww3ds.elf` byte for byte (its SHA-256 is in `ELF.sha256`).

No Nintendo copyrighted data is included. To play, you need your own *The Wind Waker* (USA, GZLE01) disc image.
