# CitroWaker port source

The New 3DS port of the Wind Waker decompilation: everything that runs on the console besides the decompiled game itself.

- `3ds/` — platform layer (`src/`: threads, audio, input, pacing, memory, GX FIFO bridge, UI), patched citro3d
  (`citro3d/`, zlib licence), build and CIA packaging scripts, and the asset-free build mode (`public/`).
- `renderer/` — the GX → PICA200 renderer (`src/` core, `src3ds/` 3DS backend, `texpack/` texture-pack tools).

No game data is included. The decompiled game (SwitchWaker) is built from its own repository; its data arrays are read from
the player's own `GZLE01` disc image at start-up (`COS_RUNTIME_ASSETS=1`, see `3ds/public/make_tree.sh`).

Building (Linux, podman, devkitARM): `bash 3ds/public/make_tree.sh`, then `N3DS_PUBLIC=1 bash 3ds/build.sh`, then
`bash 3ds/public/package_public.sh`. The scripts expect the SwitchWaker checkout at `~/ww-port/SwitchWaker` and this
renderer at `~/Downloads/ww-replay3ds/hw6`; adjust `SW`/`HW6` at the top of `3ds/build.sh`.

Status: work in progress. Known issues and remaining release gates are in `3ds/RELEASE.md`.
