# Original 3DS platform components

These are asset-free source snapshots from the port, not a complete 3DS application. No game code, SDK headers, renderer, generated game assets, executable or DSP firmware is included.

| Component | What can be used | Dependencies |
| --- | --- | --- |
| `audio_3ds.cpp` | NDSP stereo PCM16 output at 32/48 kHz, bounded linear-memory wave buffers, queued-byte accounting and shutdown | devkitARM/libctru, C++17, a caller-provided `n3dsLog` implementation |
| `n3ds.h` | Plain-C bridge declarations used by the platform | `<stdint.h>`; most declared port functions are not implemented in this subset |
| `qol_config_3ds.h` | Transactional startup-only settings parser | C++17 and standard C library; the game hooks/sensor backend are not included |
| `ui_lifecycle_3ds.h` | Packed HUD visibility/fade/block snapshot | `<stdint.h>` |
| `ui_shortcuts_3ds.h` | Fixed layout, hit testing, captured-touch cancellation and item availability | C++17; action definitions in `ui_shortcuts_api_3ds.h` |
| `ui_shortcut_config_3ds.h` | Action-only startup configuration | C++17 and standard C library |
| `ui_shortcuts_api_3ds.h` | C ABI for the full game's input/presentation bridge | Declarations only; full renderer/input integration is not included |

The host-testable headers do not require an ISO, Nintendo SDK, libctru or emulator. From the repository root, run `python3 tests/run.py` with Python 3.10+ and a C++17 compiler. `--python-only` runs just the standalone tool tests.

## Audio integration

Compile `audio_3ds.cpp` into an existing libctru application with this directory on its include path. Supply `extern "C" void n3dsLog(const char*, ...);`, link libctru and initialize the services required by your application. The source does not contain or supply DSP firmware. For this port, calls are serialized by the audio mutex; do not invoke the API from an interrupt handler.

- Open with `n3dsAudioOpen(32000)` or `n3dsAudioOpen(48000)`.
- Supply interleaved signed 16-bit stereo PCM; each complete frame is four bytes. `n3dsAudioPut` copies the input and returns zero if the whole push cannot fit.
- Call `n3dsAudioResume()` to leave the initial paused state. Keep each individual push bounded by available queue capacity.
- `n3dsAudioQueued()` returns queued bytes plus partial staging; it does not report the sample-accurate remainder of a currently playing block.
- Use `n3dsAudioShutdown()` before application teardown. Changing sample rate discards queued audio.

This file was target-compiled during the publication audit. The repository does not include a runnable audio example or claim a new hardware test of this snapshot.

## Input/UI integration boundary

`Touch::update` takes an already sampled touch state and returns an action bit. A drag outside the captured button, a blocked state or an invalidated action cancels the gesture until release. Duplicate actions keep distinct pressed slots. The caller owns input sampling and execution.

The lifecycle word packs visibility, usability and blocking for an immutable frame. These helpers do not draw a UI, move a minimap or implement gyro. They must be wired into a producer/renderer with frame-owned resources before presenting those features to players. The examples are clean defaults, not user configuration copied from an SD card.

See [source scope](../../docs/SOURCE.md) and the root MIT licence. Existing local platform/game copies remain authoritative during active development; this is a reviewed publication snapshot.
