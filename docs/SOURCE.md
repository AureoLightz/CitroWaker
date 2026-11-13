# First public source components

The repository now contains selected original implementation, rather than documentation alone. This is a reviewed snapshot, not the complete game port or a downloadable release.

## Use it now

| Path | Purpose | What you need |
| --- | --- | --- |
| [tools/setup](../tools/setup) | Windows/Linux local setup UI and SD-folder exporter | Python 3.10+; a trusted matching release manifest, C++17 compiler for the bundled converter, and separately supplied compatible CIA/material bundles for a real export |
| [tools/texture-pack](../tools/texture-pack) | Standalone ISO texture extractor and converter | Python 3.10+ and C++17 GCC/Clang; no SDK/game build |
| [platform/n3ds](../platform/n3ds) | NDSP audio, the C platform bridge, shortcut/visibility logic and QoL parser | C++17 for host headers; devkitARM/libctru plus a logging callback for the audio object |
| [tools/console-report](../tools/console-report) | Summary/comparison of console logs | Python 3, standard library only; bring your own log |
| [tools/showcase](../tools/showcase) | Measured timing overlay for existing video | Python 3 plus FFmpeg for rendering |
| [tests](../tests) | Portable host qualification | Python 3.10+ and a C++17 compiler |

From the repository root:

```sh
python3 tests/run.py
# Also build and test the native texture converter:
python3 tests/run.py --texture-tools
# Without a C++ compiler:
python3 tests/run.py --python-only
# Open the local preparation UI:
python3 tools/setup/app.py
```

Linux host tests pass, including 4,194,304 lifecycle combinations, touch gesture cancellation, duplicated action assignments and transactional parsing. Python tests use generated synthetic fixtures, not a real disc or CIA. The NDSP audio source was separately compiled with devkitARM/libctru; this does not test DSP behaviour on hardware. Windows launchers and runtime paths are supplied, but no Windows qualification is claimed.

The optional GitHub Actions template is `tests/host-tests.yml.example`. It is not installed as an active workflow: the publication login lacks the extra workflow permission. The commands above were run locally; no hosted CI result is claimed.

## Boundaries

- The setup UI opens without a disc. Completing a real export requires a compatible CIA and material bundles that this repository does not include. Extractor/converter source is now included. The example manifest is deliberately a template.
- A native texture converter is now included as source; no model converter or Nintendo-derived bundle is supplied. The setup app does not build the CIA or independently produce material bundles.
- The platform bridge contains declarations used by the complete port. Unimplemented bridge functions are not evidence of a complete linkable application here.
- UI helpers are logic/configuration, not extracted artwork or the full in-game panel. Game, renderer, sensor and producer hooks remain separate.
- No full renderer, game/decompilation implementation, Aurora SDK, build image, generated game data, firmware, music or game executable is supplied. The converter uses a narrow reviewed subset of the original texture-processing code.

## Provenance and licence

These implementation files were developed for this port, rather than copied from Nintendo code. The standalone texture extractor also vendors a specifically identified SwitchWaker MIT parser with its original notice. `SOURCE_MANIFEST.json` records original component names and SHA256 source snapshots. Publication changes to the setup copy cover visible branding and the documented input-validation checks; the production development tree was not edited. Platform source files are copied byte-for-byte. New tests/docs/CI are project-original.

The root MIT licence applies to original project source, tests, tooling, documentation and the original SVG. It does not license the gameplay imagery inside screenshots, Nintendo assets or separately supplied upstream dependencies. See [RIGHTS.md](../RIGHTS.md).

The texture converter has its own PROVENANCE.json and adjacent upstream MIT notice. See [player release progress](PLAYER_RELEASE.md).

Future renderer/game source publication needs a separate per-file provenance and upstream-licence review, plus a stable dependency/build arrangement. Do not copy the entire development workspace into this repository.
