# Performance

Status: November 12, 2026, build v92 measured on a physical New 3DS. Emulator figures are labelled as such.

## Where the time goes (console, Outset gameplay)

| Per frame | Time |
|---|---|
| Process: decoding the game's GameCube GPU command stream (one core) | ~90–100 ms |
| Render: building and submitting 3DS GPU work (another core) | ~80–90 ms |
| Game logic | ~20 ms |
| **Result** | **~10–12 FPS** |

Process and render overlap on separate cores, so a frame costs about as much as the slower of the two. A typical Outset frame carries about 15,000 GameCube commands and about 500 draws.

In an emulator profile of the same area, display-list traffic accounts for about two-thirds of process time, and nearly all of it comes from the game's model renderer (J3D).

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
