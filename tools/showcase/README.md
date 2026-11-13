# Cutscene timing overlay

A standalone Python 3 + FFmpeg tool for making a measured CitroWaker showcase clip. The requested scene is the **late-game Ganondorf encounter**. Footage of that encounter has not yet been captured or its debug boot verified. This tool annotates existing footage; it does not unlock stages, launch an emulator, alter saves, or run a benchmark.

Install FFmpeg with libass and libx264 support; `ffmpeg` and `ffprobe` must be on PATH. Python uses only its standard library. The encoder uses two CPU threads, without GPU encoding.

## Footage and timing

Supply a real video plus a CSV with one measured frame per row:

```csv
time_s,frame_ms
0.000,41.2
0.043,43.0
0.087,44.1
```

`time_s` is a synchronized timestamp in the footage. `frame_ms` is the measured presentation time, not the video file's nominal frame rate. Select the source explicitly: `virtual` for emulated-console timing, `console` for real hardware, or `host` for measured host presentation timing.

```sh
python overlay.py render ganondorf.mp4 timings.csv ganondorf-timing.mp4 --metric virtual --title "CitroWaker — Ganondorf"
```

The output adds a footer underneath the picture with current frame time, one-second rolling FPS and a six-second graph. It does not cover the game or interpolate frames. Video is display-scaled to 800 pixels wide, audio is encoded to AAC, and output is MP4. Existing outputs are never overwritten. Samples older than one second display as unavailable. Graph peaks beyond `--max-ms` are clipped visually; the numerical frame time remains unchanged.

## Existing game logs

Export a continuous per-frame `[frame] ... present ... tick ...` log:

```sh
python overlay.py export-log log.txt timing-console-clock.csv
```

**These timestamps use the console clock, not the footage's host clock.** Align them to the video first. `--offset` places CSV time zero at a known video timestamp. `--time-scale` sets video seconds per CSV second when a calibrated constant ratio is valid. Neither changes the displayed measured values. With variable emulator speed, a single linear scale is insufficient: use synchronized capture timestamps rather than assuming one. Choosing `--metric virtual` does not make virtual FPS equivalent to real-console or host FPS.

The exporter deliberately refuses 60-frame summary lines and discontinuous frame ranges. It does not invent per-frame data from averages. No automatic upload is performed.

## Validation

```sh
python -m unittest -v test_overlay.py
```

Host integration was tested with a clearly marked synthetic video and measured Outset log samples, not presented as Ganondorf footage or gameplay evidence. Windows has not been tested. A verified late-game capture fixture and correctly synchronized footage/timing are still needed for the requested showcase.
