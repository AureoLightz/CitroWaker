# Performance

Latest console measurement: build v148o on a physical New 3DS (November 2026). Emulator figures are labelled as such.

## Current state (v148)

| Console, busy Outset | v132 | v148e | v148o (busiest view, ~540 draws) |
|---|---|---|---|
| Frames per second | ~12 | ~15 (heavy village frames 7–18) | ~9–10 at 61% game speed |
| Render core (GPU work building) | ~47 ms | 50–55 ms | ~108 ms per painted frame |
| Decoder core (GameCube command stream) | ~44 ms | — | ~92 ms |

The busiest view is **renderer-bound**: the game thread never waits on anything else. On v148o the draw-replay cache was starved of memory (pinned at its 512 KB floor), so most draws took the slow path. v148s freed about 4 MB for it.

Renderer work since then (emulator, exact; console results pending):

| Change | Scene | Before → after |
|---|---|---|
| v148y particle colour segments | effect-heavy (Siren) | render 23.2 → 10.9 ms, decoder 14.6 → 11.1 ms, draw events 706 → 237 |
| v148w shape layout memo | Outset | decoder 5.61 → 5.08 ms |
| v148v cached material hashes, pooled allocations | Outset | decoder −4.3% |
| v148t SDK register shadow | Outset | game draw 5.2 → 4.1 ms/tick, decoder 12.2 → 10.5 ms |

Next target, from an emulator profile of a busy Outset view: about 45 draws per frame whose geometry is rebuilt every frame (sea surface, effects). They cannot use the replay cache and take 20–40% of render time.

## History

## Where the time goes (console, heavy Outset gameplay)

| Per frame | v92 | v132 |
|---|---|---|
| Process: decoding the game's GameCube GPU command stream (one core) | ~90–100 ms | ~44 ms |
| Render: building and submitting 3DS GPU work (another core) | ~80–90 ms | ~47 ms |
| Game logic (game thread, average) | ~20 ms | ~31 ms (heavy frames up to ~42 ms) |
| **Frame period on heavy frames** | **~100 ms (~10 FPS)** | **~80 ms (~12 FPS)** |

The target is a constant 30 FPS (33 ms per frame). Every stage above is still over that budget on heavy frames, so no single change reaches it; frame skipping is not used to fake it.

### v136: paint pacing no longer rounds down to 15 fps

The game logic always runs at 30 ticks per second; a tick is drawn when the renderer can take it. Until v135 the drawing cadence was a whole number of ticks (every tick = 30 fps, every 2nd = 15 fps), so a renderer needing 34–66 ms per frame always gave 15 fps. v136 paints whenever the renderer's measured cost has been earned (a 40 ms renderer gives 25 fps: 5 of every 6 ticks drawn). On console v132 the game thread slept more than half of the time in those 15 fps windows, so the renderer, not the game, was the limit.

| Emulator simulation, render cost raised to console level (+30 ms) | Presented | Game speed |
|---|---|---|
| v135 whole-number cadence | 15.0 fps | 100% |
| v136 fractional pacing | 25.0 fps | 100% |

Console result pending.

Process and render run on separate cores, but they still wait on each other and on the game thread, so the heavy-frame period (~80 ms) is longer than either stage alone. A typical Outset frame carries about 15,000 GameCube commands and about 500 draws.

In an emulator profile of the same area, display-list traffic accounts for about two-thirds of process time, and nearly all of it comes from the game's model renderer (J3D).

### v137–v138: less game-thread work per tick

All exact (the game behaves the same; the GX command stream is byte-identical):

- `MTXConcat` reads its 24 inputs once instead of re-reading them after every store (about 3x fewer instructions; the J3D joint walk's top cost).
- Game code is built without a frame pointer.
- GX FIFO writers (`GXPosition3f32`, `GXCmd1u32`, ...) are inlined into the game: one bounds check and a store instead of two calls per value.
- The compiler no longer zero-fills joint and 2D pane matrices that are completely written before use.
- A per-frame busy loop in `JFWDisplay::calcCombinationRatio` (one step per video interval) is now one division.
- Collision scans (wall, ground, roof, line, sphere) stop at the last used collision slot instead of walking all 256.
- The bottom-screen panel's static left side is drawn once and copied, instead of redrawn with its text whenever the FPS readout changes.

| Emulator, Outset, game-thread ms per tick (two runs each) | v136 | v138 |
|---|---|---|
| Actor update (`execute-all`) | 4.80 | 4.17 |
| Collision (`bg-acch`) | 1.16 | 0.90 |
| J3D joint/matrix work (`J3D-calc`) | 1.38 | 1.14 |
| Model draw lists (`J3D-drawBuffer`) | 1.98 | 1.67 |
| Painter | 5.42 | 4.86 |

The time saved shows up as waiting at frame end for the renderer (`after-draw` 0.6 → 1.5 ms), so in this scene the renderer side is now the limit.

**Console (v138, heavy Outset gameplay):** still about 10–13 fps (80–100 ms per presented frame); light stretches reach 26–30 fps. Per heavy frame: GX decode ("process") 59–73 ms, renderer 72 ms (draw submission 48 ms), and the game thread waits 17–22 ms per painted tick for the decoder to finish. Both decode and render are about twice what 25 fps allows, so the game-thread savings alone do not show up on hardware yet.

### v139: allocator fallbacks

The console log showed the per-thread small-block allocator running out of its span budget once gameplay starts, after which 30,000–165,000 allocations per 60 frames went to newlib's locked heap; the heaviest windows had the most. v139 hands out a free block from a larger size class instead (exact: a block always returns to the class it came from). v139 also carries a one-session console A/B (`N3DS_AB_KNOBS=1`) for this and two other exact options. Console result pending.

## What has been tried

| Change | Result |
|---|---|
| Retained geometry: unchanged batches are not decoded again | Kept. About 70% of draw decoding skipped in steady state. |
| Resident geometry: unchanged packed buffers stay in GPU memory | Kept. |
| Allocation pools, plan reuse, matrix grouping, specialised vertex decode | Kept. Emulator frame 54 → 38 ms. |
| Offline texture pack: textures converted on a PC, read on the console | Exact, but break-even on hardware: 6.4 ms per read vs 5.6 ms per conversion. No FPS change. |
| Precompiled material state (first slice of the model path) | Slightly slower in the emulator (38.6 vs 37.6 ms). Off. |

The two last rows have a common lesson: the renderer already caches most of what a precompute layer would provide. Speed has to come from removing work, not from preparing the same work earlier.

## Plan: direct model path

The model renderer currently describes every model as GameCube commands. The game thread encodes them, and another core decodes them again. The plan takes model draws out of that stream:

1. **PC model converter.** Read every model on the player's own disc image. Write a model pack with ready-to-use 3DS vertex/index buffers per shape and precompiled 3DS materials.
2. **Game side.** For each shape, hand the renderer "shape #, material #, matrices, animated material values" instead of display lists. Unsupported cases (CPU-deformed meshes, for example) keep the current path.
3. **Renderer.** Draw those records directly, in order with all other draws, using the frame's depth and fog settings.
4. **Verification.** Render accepted shapes both ways in the emulator and compare pixels before enabling it by default. Then measure on the console.

## Measuring

Builds log per-frame timings to `/3ds/ww3ds/log.txt` (`[present]`, `[3ds] frame`, `[render-split]` lines). Emulator ("virtual") FPS is not console FPS. Azahar has been about 3× optimistic for heavy frames.
