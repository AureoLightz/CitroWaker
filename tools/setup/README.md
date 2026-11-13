# CitroWaker Setup

Source implementation of a local Windows/Linux setup application for CitroWaker. Uses Python3.10+ and the standard library; the interface opens in your browser, with native file pickers when available. No Tk, account, external website or upload is required. No Nintendo artwork is used by the application UI.

**This checkout does not include a compatible release bundle or its converter/extractor/CIA.** You can run the app and tests now; a real export requires those companion files.

## Run

Linux: `bash launch.sh`. Windows with Python3 installed: double-click `launch.cmd`. Or run `python app.py`. Keep the launcher running until export completes; Ctrl+C cancels active work. The local interface uses an ephemeral127.0.0.1 port with a random request token. Do not share its session URL. Without a native picker, paste full paths.

Select your USA GameCube ISO, a compatible release.json, and a NEW export directory. Preparation copies the ISO to a temporary staging directory, extracts textures with the release's actual tool, converts/verifies them, copies the CIA/material bundles and writes INSTALL.txt +PREPARATION.json SHA256 checksums. The source ISO is unchanged. Failed/cancelled preparation removes staging; existing exports are rejected. No SD card or existing saves are edited.

The finished folder contains `cia/CitroWaker.cia` and `3ds/ww3ds/{GZLE01.iso,ww_texpack.bin,ww_bundle*.bin,user/}`. Copy to your SD, preserving existing saves/configuration/misses, install the CIA separately and launch from HOME Menu. No DSP firmware is supplied.

## What this version does not do

- Build the3DS executable or CIA from scratch: it packages a compatible prebuilt CIA supplied by the selected release.
- Produce offline model packs or direct model-drawing data: those converter stages do not exist here yet.
- Independently generate the material bundles from the ISO: the release manifest currently points to locally prepared bundles.
- Guarantee whole-game texture coverage, rendering correctness or a framerate. Missing/unexpressible textures retain the runtime fallback; pack verification compares with the current converter, not original GameCube pixel fidelity.
- Include a redistributable release, game data, tools' native binaries, or Nintendo banner/icon/music assets.

## Release bundle contract

See release.example.json. Paths are relative to the manifest; absolute development paths also work. Use ONLY a trusted compatible release: its converter is an executable and its extractor/parser are Python code. This app does not download or authenticate a release. The creator must provide audited tools/native binaries and a compatible CIA. Windows needs a Windows `mktexpack.exe` (change the manifest path); a Linux binary cannot be reused there.

The current extractor imports disc_manifest from a developer-specific directory. The application's isolated extraction worker explicitly loads the selected parser first, so this app does not require the user's home directory to resemble a developer checkout. No upstream extractor is modified.

This original setup implementation is MIT-licensed under the repository root LICENSE. Companion releases retain their own rights and require a separate audit.

A portable distributable still needs per-platform packaging/testing, a licensed tool-source audit, release hashes/signatures, locally generated or original CIA banner assets, and a public material-preparation workflow. Do not put the real generated texture pack, ISO, current Nintendo-art CIA or game-derived bundles into GitHub. This source folder contains only implementation/UI/documentation and a placeholder manifest.

## Evidence

`python -m unittest -v test_prepare.py` covers export tree/checksums, cancellation/failure cleanup, invalid ISO headers, unsafe/duplicate bundle names and existing output preservation. A real Linux developer export extracted5049textures, generated24290texture records, verified0badrecords and assembled the SDtree. Windows and the browser UI still need interactive qualification; no Windows executable is claimed tested.
