# CitroWaker Setup

Source implementation of a local Windows/Linux setup application for CitroWaker. Uses Python3.10+ and the standard library; the interface opens in your browser, with native file pickers when available. No Tk, account, external website or upload is required. No Nintendo artwork is used by the application UI.

**The texture extractor/converter sources are now included.** Setup can build the converter automatically with a C++17 compiler. A compatible CIA and locally prepared material bundles are still required; the current developer CIA is not cleared for public redistribution.

## With an existing local build

`create_manifest.py` creates a local manifest from an existing CIA and base/title bundles, without copying them or declaring them redistributable. See [the player-install guide](../../docs/PLAYER_RELEASE.md) for the complete command and update/save precautions.

## Run

Linux: `bash launch.sh`. Windows with Python3 installed: double-click `launch.cmd`. Or run `python app.py`. Keep the launcher running until export completes; Ctrl+C cancels active work. The local interface uses an ephemeral127.0.0.1 port with a random request token. Do not share its session URL. Without a native picker, paste full paths.

Select your USA revision-0 GameCube ISO, a compatible release.json, and a NEW export directory. Preparation copies the ISO to a temporary staging directory, extracts textures with the release's actual tool, converts/verifies them, copies the CIA/material bundles and writes INSTALL.txt +PREPARATION.json SHA256 checksums. The source ISO is unchanged. Failed/cancelled preparation removes staging; existing exports are rejected. No SD card or existing saves are edited.

The finished folder contains `cia/CitroWaker.cia` and `3ds/ww3ds/{GZLE01.iso,ww_texpack.bin,ww_bundle*.bin,user/}`. Copy to your SD, preserving existing saves/configuration/misses, install the CIA separately and launch from HOME Menu. No DSP firmware is supplied.

## What this version does not do

- Build the3DS executable or CIA from scratch: it packages a compatible prebuilt CIA supplied by the selected release.
- Produce offline model packs or direct model-drawing data: those converter stages do not exist here yet.
- Independently generate the material bundles from the ISO: the release manifest currently points to locally prepared bundles.
- Guarantee whole-game texture coverage, rendering correctness or a framerate. Missing/unexpressible textures retain the runtime fallback; pack verification compares with the current converter, not original GameCube pixel fidelity.
- Include a redistributable release, game data, tools' native binaries, or Nintendo banner/icon/music assets.

## Release bundle contract

See release.example.json. Paths are relative to the manifest; absolute development paths also work. Use ONLY a trusted compatible release: its converter is an executable and its extractor/parser are Python code. This app does not download or authenticate a release. The example uses `"texture_tools": "bundled"`, selecting the public extractor/parser and building a native converter when missing or stale. With a C++17 GCC/Clang compiler (MinGW-w64 on Windows), no developer-path tools are needed. Legacy manifests can instead name extractor/disc_parser/texture_converter paths. A Linux converter cannot be reused on Windows. The creator must still supply a compatible locally built CIA and bundles.

The public extractor uses its colocated MIT-licensed SwitchWaker parser. The application also preloads the selected parser for legacy manifests. No native game/compiler SDK is needed to build the PC converter.

This original setup implementation is MIT-licensed under the repository root LICENSE. Companion releases retain their own rights and require a separate audit.

A portable distributable still needs per-platform packaging/testing, release hashes/signatures, an asset-clean CIA with complete corresponding source, and a public material-preparation workflow. Do not put the real generated texture pack, ISO, current Nintendo-art CIA or game-derived bundles into GitHub. This source folder contains only implementation/UI/documentation and a placeholder manifest.

## Evidence

`python -m unittest -v test_prepare.py` covers export tree/checksums, cancellation/failure cleanup, invalid ISO headers, unsafe/duplicate bundle names and existing output preservation. A real Linux developer export extracted5049textures, generated24290texture records, verified0badrecords and assembled the SDtree. Windows and the browser UI still need interactive qualification; no Windows executable is claimed tested.
