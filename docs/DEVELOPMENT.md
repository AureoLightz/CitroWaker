# CitroWaker

**An experimental native port of The Legend of Zelda: The Wind Waker to New Nintendo 3DS.**

CitroWaker adapts the original GameCube game through the Wind Waker decompilation, SwitchWaker and Aurora, with a renderer built for the 3DS graphics hardware. The aim is to preserve the game’s appearance and behaviour while making it practical to play on a handheld—with sensible controls, an optional touchscreen interface and quality-of-life features.

**This is an active development project, not a finished release.** It boots on real New Nintendo 3DS hardware, but performance and graphical correctness still need substantial work. Completing the whole game has not been verified.

**Repository scope:** selected original platform components, a local setup app, reporting/showcase tools and host tests are published. See [source scope](SOURCE.md). The full game, renderer, build scripts and CIAs are not included; cloning this repository alone cannot build or run the complete port. The full-port arrangement described below remains in the local development tree.

No game disc image, extracted Nintendo assets or Nintendo music is provided by this repository. You must supply your own compatible game data.

Status reviewed against the local source and packaging artifacts on **November 12, 2026**. Build-specific validation is noted separately.

## What works—and what still needs work

| Area | Current status |
|---|---|
| Native game execution | Boots and reaches gameplay on New 3DS and in Azahar. |
| Audio | Output is implemented through the 3DS DSP. Emulator slowdowns can also affect audio playback. |
| Controls | Circle Pad movement, C-stick camera and configurable physical button mappings are implemented. |
| Saves | The game uses an SD-backed memory-card implementation. Save-related failures have been reported; keep backups. |
| Rendering | The GX-to-PICA renderer is operational. On real hardware, fogged scenes draw far geometry (ocean, cliffs) over near geometry: the console's W-buffer depth mode stores far smaller values than Azahar does. A Z-buffer experiment (`depth_z=1`, fog off) is being tested on hardware; see [docs/CONSOLE_FINDINGS.md](CONSOLE_FINDINGS.md). A noisy cloud strip on the horizon is also open (texture data has been ruled out). |
| Performance | About 10–12 FPS in Outset gameplay on a New 3DS (build v92). Model draws going through the emulated GameCube command stream are the main cost; a direct model path is in development. See [docs/PERFORMANCE.md](PERFORMANCE.md). |
| Bottom-screen map | An experimental routing backend exists. The redesigned shortcut panel and its transitions are not yet a finished player-facing feature. |
| Optional QoL | A startup configuration loader is implemented for existing sailing, grappling, song-replay and text hooks; gameplay validation is pending. |
| Gyro aiming | Sensor and aiming functions are linked in the current local packaged ELF. Physical direction, drift and aiming behaviour remain unvalidated on hardware. |
| Top-screen HUD fitting | A default-off source candidate exists; final placement and cost need in-game validation. |

“Implemented in source” does **not** mean a feature is present in every previously distributed CIA. Check the notes accompanying the specific build you install.

## Hardware and game requirements

- A **New Nintendo 3DS-family system** capable of installing and running homebrew CIAs. Original 3DS/2DS hardware is not a supported target.
- A compatible, personally supplied **USA GameCube disc image, game ID `GZLE01`**, named `GZLE01.iso`.
- An SD card with space for the disc image, locally prepared material bundles and saves.
- DSP firmware obtained from your own console where required by the audio setup. It is not included here.

Azahar is useful for development, but it does not reproduce every hardware limit or GPU behaviour. An emulator result is not a real-console performance or correctness guarantee.

## Installation

The supported delivery format for this project is **CIA**. There is no public release URL specified yet; these instructions apply to a build you have obtained or prepared yourself.

1. Prepare the SD data directory shown below using your own game data and locally generated bundles.
2. Copy the CIA to the SD card—for example, `/cia/ww3ds.cia`.
3. Install it using your CIA installer.
4. Exit the installer and launch the installed title from the HOME Menu.
5. Keep a backup of `/3ds/ww3ds/user/`, especially before updating an experimental build.

```text
SD card
├── cia/
│   └── ww3ds.cia
└── 3ds/
    ├── dspfirm.cdc                 # If required; supplied by the player
    └── ww3ds/
        ├── GZLE01.iso              # Player’s own compatible disc image
        ├── ww_bundle.bin           # Locally prepared material bundle
        ├── ww_bundle_title.bin     # Locally prepared title material bundle
        ├── ww_bundle*.bin          # Any additional locally prepared bundles
        ├── ww_bundle_misses.bin    # Created/grown by the game
        ├── ww_texpack.bin          # Optional: textures converted on a PC (about 289 MB); no measured speed gain yet
        ├── user/                  # Saves: preserve when updating
        ├── buttons.ini            # Optional physical remapping
        ├── shortcuts.ini          # Optional shortcut assignments; feature-dependent
        ├── qol.ini                # Optional QoL settings; feature-dependent
        └── performance.ini        # Build-specific performance settings
```

The CIA does not replace the SD data directory. Keep the ISO, saves, bundles and personal configuration when updating the executable. Do not overwrite an existing save directory with an empty one.

Bundle preparation is still part of the development workflow; a clean public, end-to-end asset preparation process is not yet complete. Do not download or commit somebody else’s extracted game data to fill that gap.

`env.txt`, `autopress.txt` and `shots.txt` are development controls, not normal package contents. A leftover testing file can change startup behaviour or inject input. This repository must not distribute a player’s copies of those files.

## Controls

The default physical mapping is:

| New 3DS input | GameCube action |
|---|---|
| Circle Pad | Control Stick / movement |
| C-stick | C Stick / camera |
| A, B, X, Y | A, B, X, Y |
| L, R | Full L, R trigger presses |
| ZL or ZR | Z |
| START | START / pause |
| D-pad | D-pad |
| SELECT + START | Z, suppressing the individual START action |

SELECT alone is unassigned as a game action; development builds use it to save a diagnostic capture (colour, depth and, on the first press, the textures in use) to `/3ds/ww3ds/`. Consult that build’s notes.

Physical remapping uses `/3ds/ww3ds/buttons.ini`. For example:

```ini
A=B
B=A
SELECT+START=NONE
```

Restart after editing. A physical mapping is separate from a touchscreen shortcut’s semantic action.

### Touchscreen interface

The intended layout keeps a live map on the bottom screen, three customizable item shortcuts on the right, and **Menu** / **Map zoom** along the bottom. The footer actions and layout stay fixed; only the three item actions are customizable.

```ini
# /3ds/ww3ds/shortcuts.ini
slot1.action=X
slot2.action=Y
slot3.action=NONE
```

An item shortcut uses the item actually equipped to that game slot. `NONE` leaves the shortcut inactive. The UI is intended to follow gameplay visibility and HUD opacity, clear during blocked contexts, and cancel a held touch across transitions.

The configuration and presentation code are implemented and host-tested, but the latest panel still needs renderer integration and ARM validation. Reliable capture of short taps between very slow game frames is also unfinished. Do not assume the screenshots of design previews represent a running build.

See [SHORTCUTS.md](../SHORTCUTS.md) for the current configuration contract.

## Optional quality-of-life features

Original behaviour remains available. The current source accepts an optional `/3ds/ww3ds/qol.ini`:

```ini
sail=swift
fast_grapple=1
no_song_replay=1
instant_text=0
gyro=0
gyro_sensitivity=100
gyro_invert_x=0
gyro_invert_y=0
```

- `sail=original`: original sailing.
- `sail=swift`: the existing faster-sailing and automatic-wind hooks.
- `sail=wind`: automatic wind without extra sailing speed.
- `fast_grapple=1`: enables the existing faster grappling-hook option.
- `no_song_replay=1`: skips the existing conducting replay after a song.
- `instant_text=1`: enables the existing faster-text option.
- `gyro=1`: opts into the source gyro-aiming implementation; hardware qualification is still pending.
- `gyro_sensitivity`: integer percentage, from `25` to `300`.
- `gyro_invert_x` / `gyro_invert_y`: motion inversion, independent of stick inversion.

These recreate selected conveniences through existing game hooks; they are not a wholesale backport of Wind Waker HD. There is no implemented HD-style quest-shortening feature advertised here.

The file is read once at startup. A missing file changes nothing; malformed input rejects the whole profile. A valid file resets omitted supported options to their original defaults. See [QOL.md](../QOL.md) for exact behaviour and outstanding validation.

## Performance and fidelity

Performance work targets less decoding, copying, allocation and repeated GPU preparation. The goal is to preserve effects, geometry, textures and animation—not trade them away for an inflated frame-rate number.

Reported numbers must distinguish:

- **Console FPS:** measured on a physical New 3DS.
- **Emulated/virtual FPS:** timing inside the emulated console.
- **Host FPS and emulation speed:** what the emulator actually delivers on the PC.

These are not interchangeable. On a physical New 3DS, Outset gameplay runs at about **10–12 FPS** (build v92), while Azahar reports about 26 virtual FPS for a comparable scene. Earlier builds ran heavy Outset scenes at 3–6 FPS on hardware. [docs/PERFORMANCE.md](PERFORMANCE.md) has the per-frame breakdown, what has been tried, and the plan.

Current structural work includes retained decoded geometry, resident packed buffers, allocation pools and memory reductions intended to preserve overlap between processing and rendering. Experimental settings are build-dependent; avoid copying an old `performance.ini` wholesale into a newer build.

See the development performance log (not included in this documentation and showcase-tools repository yet) for development evidence. Private candidates and PC-only memory measurements are not shipped performance claims.

## Building from source

**The current working tree is not yet a self-contained, reproducible public checkout.** Build scripts still expect separately prepared source trees, generated game data and local tools. This section documents the existing development arrangement rather than pretending a fresh clone is sufficient.

The build uses:

- The SwitchWaker game and native sources, including the fetched Aurora dependency.
- The project’s separate GX-to-3DS renderer source tree.
- Clang for game/Aurora units.
- devkitARM, libctru and citro3d for the 3DS platform and renderer.
- Podman with the locally prepared `localhost/ww3ds-clang` image.
- `makerom`, `bannertool` and `ctrtool` for CIA packaging/inspection.

The existing scripts expect these locations:

```text
~/ww-port/SwitchWaker/
~/ww-port/n3ds/
~/Downloads/ww-replay3ds/hw6/
```

After preparing the dependencies and local game assets:

```bash
cd ~/ww-port/n3ds
bash build.sh
bash package_cia.sh
```

`build.sh` produces the ELF and an internal `.3dsx` development artifact. CIA packaging consumes a stable copy of the existing ELF and writes `cia/ww3ds.cia`; it does not compile the game. CIA is the player-facing delivery format.

The current packaging path requires locally prepared banner/icon resources, which are not suitable for committing when they contain Nintendo artwork or music. Public packaging needs original replacement assets or local extraction steps before it can be called reproducible.

## Contributing

Useful contributions include hardware bug reports, reproducible scene tests, renderer correctness, memory profiling, input behaviour and build reproducibility.

Before changing shared code:

1. Agree on ownership in the project’s local coordination log (not uploaded here).
2. Keep changes focused and preserve existing work.
3. Compare output against the appropriate current baseline.
4. Identify whether evidence comes from PC replay, ARM emulation or hardware.
5. Record changed files, results and remaining limitations.

Development rules include one build at a time and one emulator at a time. Builds use the shared build lock; emulator tests use the shared Azahar lock and capped runners. Never terminate another worker’s emulator or modify their testing configuration without coordination.

For a bug report, include the build identifier, console/emulator details, scene and steps to reproduce. State whether it happens consistently. Logs, photographs and crash dumps can help, but review them for personal information before sharing. Do not attach game images, saves containing personal information, extracted assets or entire SD-card archives.

## Repository contents and licensing

Original components published here use the root MIT licence; that does not establish a licence for the entire local port. Upstream components retain their own licences and notices. The November 13 publication audit and subsequent component-source review cover the files actually published here only. The local port source, packaging assets and generated game data require their own audit before any later upload. See [scope and rights](../RIGHTS.md).

Keep out of the public repository and its history:

- Disc images and extracted Nintendo textures, models, fonts, artwork and music.
- Game-derived material bundles and generated asset payloads.
- Current Nintendo-art banner/icon files and title-music clips.
- Saves, DSP firmware, SD-card contents and personal testing configuration.
- Local build products, dependency downloads, private logs and crash dumps.

Keep the project’s implementation, permitted patches, build tooling, original documentation and the notices required by its dependencies. Prefer local extraction from the player’s own compatible disc to redistributing game assets. Do not assume that generated files are safe to publish just because a script produced them.

The main project page includes development gameplay screenshots. Disc images, extracted asset payloads and music are not distributed.

## Acknowledgements

CitroWaker builds on the work of:

- [zeldaret/tww](https://github.com/zeldaret/tww), the Wind Waker decompilation.
- [SwitchWaker](https://github.com/centollOS/SwitchWaker), the native port this project adapts.
- [Aurora](https://github.com/encounter/aurora), the GameCube platform and graphics abstraction layer.
- [devkitPro](https://devkitpro.org/), including devkitARM, libctru and citro3d.
- [Azahar](https://github.com/azahar-emu/azahar), used during development and testing.

The Legend of Zelda and The Wind Waker belong to Nintendo. CitroWaker is an independent community project and is not affiliated with or endorsed by Nintendo.
