# Real New 3DS vs. Azahar: findings

Things the physical console does differently from the Azahar emulator, found while bringing Project Windfall up on hardware. Each was invisible in the emulator, so an emulator pass alone cannot rule any of them out. They may be useful to other 3DS homebrew developers.

Status: November 12, 2026. Each entry says how it was found and how sure we are.

## GPU depth

**Writes to the depth-map scale/offset registers had no effect in our tests.** We set `GPUREG_DEPTHMAP_SCALE` / `GPUREG_DEPTHMAP_OFFSET` (0x4D / 0x4E) through `C3D_DepthMap` and with direct register writes. Reading them back through GSP returned the values we wrote, yet every depth stored on the console matched citro3d's default map (scale −1, offset 0). Azahar honours the registers. Measured with a boot-time probe that draws known depths and reads the D24S8 buffer back.
*Workaround in use:* fold the map into the vertex shader's output z (`z' = −(s·z + o·w)`) and leave the registers at the defaults.

**Z-buffer mode (`z/w`) matches the documented formula exactly.** Every Z-mode probe value matched the expected value.

**W-buffer mode does not store what Azahar stores.** In a fogged gameplay frame, the console stored the whole visible scene in roughly 170 distinct depth values, about 1/100 of the emulator's values for the same view. Near and far geometry then compete for the same few values, and in Wind Waker the ocean and distant cliffs get drawn over Link and the dock. A broader probe sweep is in progress to characterise the hardware function.

**Depth values at 1.0 or above wrap to 0 instead of clamping** (observed in v83). Far geometry then lands in front of everything. Every depth map now stays below 1.0.

## Memory and system calls

- **Never pass VRAM to `fwrite`.** The SD write is done by the FS process, which cannot map VRAM. The result is a kernel data abort on behalf of `fs`. Copy into heap memory first.
- **Never free a shader program that citro3d may still have bound.** `C3D_BindProgram` reads the previous program's data on the next bind. Azahar happened to keep the freed bytes; the console crashed on the first draw.
- **stdio files on the SD need a large buffer.** With newlib's default 1 KB buffer, writing a 6.6 MB diagnostic file became thousands of small SD writes and stalled the game for about 60 seconds. `setvbuf(f, nullptr, _IOFBF, 64 << 10)` fixes it.
- **SD read throughput in game was about 2.3 MB/s,** while the game was also streaming from the disc image. Reading pre-converted data is not automatically faster than converting it on the console.

## Threads and boot

- **Core 1 refuses threads above priority 0x30,** and every refused `svcCreateThread` uses up one of the 32 thread slots. Do not retry in a loop.
- **The 124 MB memory mode needs `NS_RebootToTitle(..., 6)`** when the title is not started from the HOME Menu.
- **Enabling texture units 1/2 with no texture coordinates hangs the GPU** on the first textured draw. Write the real unit mask after citro3d's context update.
- **A 3D model (CGFX) banner crashed the HOME Menu.** Use an image banner.

## Performance

Azahar's per-frame timings were a poor predictor of console timings for this workload. A typical heavy frame cost about 3× more on the console. Two optimisations that looked neutral or positive in the emulator did not help on hardware. Decisions about performance are made from console logs.
