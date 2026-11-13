# Real New 3DS vs. Azahar: findings

Things the physical console does differently from the Azahar emulator, found while bringing CitroWaker up on hardware. Each was invisible in the emulator, so an emulator pass alone cannot rule any of them out. They may be useful to other 3DS homebrew developers.

Reviewed November 13, 2026. These are observations from this port and its tested builds, not universal hardware guarantees. Earlier explanations were hypotheses unless independently confirmed.

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

- **Core-1 priority and thread-limit failures were observed during bring-up.** Avoid repeatedly retrying a failed placement; the port falls back after a refusal. The reported slot exhaustion does not by itself establish a universal kernel leak on every failed `svcCreateThread`.
- **Some launches received only 64 MB despite the extended-memory header.** A reboot-into-title fallback recovered the larger memory allocation in the tested runs. This does not mean every non-HOME launch universally requires that fallback.
- **Unused texture units were investigated as a GPU-hang hypothesis, then ruled out by a subsequent console test.** Reducing the enabled-unit mask did not resolve that hang. Do not cite this earlier theory as an established cause.
- **The earlier custom CGFX banner crashed the HOME Menu.** The packaging fallback uses the standard image-banner template. This finding concerns that particular asset, not all 3D HOME banners; a new custom conversion needs real-HOME validation.

## Performance

Azahar's per-frame timings were a poor predictor of console timings for this workload. A typical heavy frame cost about 3× more on the console. Two optimisations that looked neutral or positive in the emulator did not help on hardware. Decisions about performance are made from console logs.

## Latest reviewed console result: v93

With `depth_z=1` (fog disabled), the observed colour capture places the dock, Link and island correctly in front of the sea. The rainbow-speckle horizon band remains open. The SELECT depth readback was suspected to contain stale memory and is being corrected; do not derive a hardware formula from that image. This is a scene-specific result, not full-game rendering validation.
