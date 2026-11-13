# Player installation: progress and remaining release gates

The PC texture-preparation path now works from this repository. A public game CIA is still being prepared; the current private developer CIA must not be uploaded as an asset-free release.

## Working now, for an existing compatible local build

Requirements: Python 3.10+, a C++17 GCC/Clang compiler (MinGW-w64 on Windows), your USA GameCube revision-0 ISO, a locally built compatible CIA and locally prepared base/title material bundles.

1. Clone/download this repository.
2. Create a **private local manifest**, using your existing compatible files:

   ```sh
   python3 tools/setup/create_manifest.py --cia /private/CitroWaker.cia \
     --bundle /private/ww_bundle.bin --bundle /private/ww_bundle_title.bin \
     --output /private/release.json
   ```

3. Start `python3 tools/setup/app.py`. Select your ISO, that manifest and a new export folder. The bundled converter is built on first use or when its source is newer than the binary.
4. Wait for conversion and verification. The completed folder contains:

   ```text
   cia/CitroWaker.cia
   3ds/ww3ds/GZLE01.iso
   3ds/ww3ds/ww_texpack.bin
   3ds/ww3ds/ww_bundle*.bin
   3ds/ww3ds/user/
   INSTALL.txt
   PREPARATION.json
   conversion.log
   ```

5. For a first install, copy `cia/` and `3ds/` to the SD card, install the CIA with your homebrew installer and launch from HOME Menu. Follow that build's notes about console memory mode and DSP setup.

**Updates:** back up and preserve the existing `3ds/ww3ds/user/`, misses cache and configuration. Never overwrite an existing save directory with the export's empty `user/`. The preparation tool never writes the SD card or installs a title itself. No firmware is supplied.

The manifest contains your local paths and the export contains game-derived data: neither is for uploading to GitHub or sharing as a release. The tool runs local code/converters, so select only a trusted release manifest.

## What has actually been tested

Linux: the public extractor produced 5,049 unique textures from the local test ISO, with no reported parse errors. The complete extracted file matched the previous extractor byte-for-byte. The standalone converter's full 24,290-record pack matched the previous converter byte-for-byte and verified with zero bad records. A complete private SD export passed five independent payload SHA256 rechecks. No existing saves or SD data were modified.

Native converter tests also check synthetic data and deterministic output with one/two workers. Windows source/launchers are present but Windows execution has not been qualified. This is not a fresh-console game test or a new performance result.

## Before a downloadable public CIA

The current packaged ELF contains exact copies of extracted game-data arrays; changing its banner alone does not remove them. The upstream runtime-asset-stub/loader path is the intended replacement. An isolated runtime-loading build now compiles. The first emulator smoke test reproduced all 163 generated arrays and five additional DSP/font tables byte-for-byte; a follow-up build also moves six inline texture/display-list payloads to runtime loading. Its known-raw-header scan finds zero matches among 96 checked arrays. This narrow scan is not an exhaustive asset audit, and normal gameplay, presentation resources and the complete source release still need qualification.

The public artifact also needs original or locally loaded bottom-screen/HOME resources, complete corresponding-source/dependency notices for the GPL DSP HLE, and a matching material-preparation route. A texture pack is not a material bundle. These are tracked release requirements, not optional steps the installer silently skips.

Until those are complete, there is no honest “download this CIA and play” link. The working tools remain usable with a compatible local build. This guide will be updated as the release gates are closed.
