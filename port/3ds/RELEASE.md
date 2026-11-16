# CitroWaker release checklist

Single place for what stands between the current port and a release. Details live in RELEASE_ASSET_AUDIT.md,
BRIEF_public_release_runtime_assets.md and ChatGPT's work/player-release/STATUS.md. Update this file when a gate moves.

## Asset-free binary (required for any distributed CIA)

- [x] Isolated public source tree: `bash public/make_tree.sh` -> ~/ww-port/public/SwitchWaker (163 stub asset headers,
      11 inline payloads moved to runtime loading by tools/release/runtime-assets; production tree never written).
- [x] Build mode: `N3DS_PUBLIC=1 N3DS_PGO=use bash build.sh` -> public-build/ww3ds.elf (own objects; COS_RUNTIME_ASSETS=1).
- [x] Public build links and boots on the emulator: Outset free play renders like the private build; log "174 arrays read from the disc". (Title/save-load not yet run on it.)
- [x] Asset scan of public-build/ww3ds.elf: 0 of 96 known raw arrays (private ww3ds.elf fails the same scan).
- [ ] Console boot of the public CIA.

## HOME menu presentation

- [x] Original banner/icon generator (tools/release/original-home), original synthesized chime (presentation/home_audio).
- [x] (font: Noto Serif Bold, SIL OFL) `bash public/package_public.sh` -> public-build/CitroWaker.cia: original art, no Nintendo boot logo, 124 MB mode check.
- [ ] Console check: HOME menu shows the banner/icon, chime plays, no HOME crash.

## Source and licences

- [ ] Corresponding source kit for the GPL dsp_hle component, notices, build instructions (ChatGPT staged a partial kit:
      work/public-runtime-integration/source-kit; incomplete).
- [ ] Reproducible toolchain description (container image ww3ds-clang, devkitARM versions).

## Player setup

- [x] PC setup tool prepares the texture pack from the player's own ISO (Linux qualified).
- [ ] Material bundles (ww_bundle*.bin) prepared locally from the player's disc, or proven unnecessary.
- [ ] Windows setup qualified.

## Game quality (console)

- [ ] v128 (console, short session): draw-replay budget back to 4 MB (was pinned at 512 KB), reuse 51% -> 67%.
- [ ] Frame rate: about 15 fps at the real CPU clock in the heavy Outset scene (ChatGPT, Nov 15); target 30.
- [ ] Known visual bugs: grey untextured floating quad / blob on Outset (ChatGPT decoded a capture: draw ID 160 idx6 tev1, no
      texture); boat grey fragment. v128 SELECT captures: bench/console/v128/captures.bin.
- [ ] Map UI split: on the LiveMap bottom-screen setting the map moves to the bottom screen but its frame/UI stays on the top
      screen (ChatGPT: full GCN atlas/sidebar receiver not integrated yet).
- [ ] Title-screen audio repeats like a broken record (user, v128). NDSP output is healthy in the log (empty-checks 4,
      rejected 0, steady 32 kHz), so the repeat is upstream: suspect the game's music stream (ISO -> ARAM ring) reads falling
      behind during the heavy title scene. Next: log stream read latency (JASAramStream / DVD reads) at the title.
- [ ] Save/load, settings menu and audio verified on console with the release build.

NOTHING IS PUBLISHED. The user decides when; performance and bugs come first (user, Nov 15).
