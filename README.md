<p align="center">
  <img src="assets/citrowaker-header.svg" alt="CitroWaker — The Great Sea. Two screens." width="100%">
</p>

<p align="center">
  <strong>A familiar sea. A new handheld home.</strong><br>
  An experimental native port of <em>The Legend of Zelda: The Wind Waker</em> for the New Nintendo 3DS family.
</p>

<p align="center">
  <a href="#install-in-three-steps"><b>Install</b></a> &nbsp; · &nbsp;
  <a href="#the-bottom-screen">Bottom screen</a> &nbsp; · &nbsp;
  <a href="#development-status">Status</a> &nbsp; · &nbsp;
  <a href="docs/PERFORMANCE.md">Performance</a> &nbsp; · &nbsp;
  <a href="docs/DEVELOPMENT.md">Development guide</a>
</p>

<p align="center">
  <img alt="New 3DS family" src="https://img.shields.io/badge/console-New%203DS%20%C2%B7%20New%202DS%20XL-1d4f5c">
  <img alt="Installer for Windows and Linux" src="https://img.shields.io/badge/installer-Windows%20%C2%B7%20Linux-c9b673">
  <img alt="Bring your own USA disc" src="https://img.shields.io/badge/disc-your%20own%20GZLE01-30535d">
  <img alt="MIT licence for project code" src="https://img.shields.io/badge/licence-MIT-0e2530">
  <a href="https://github.com/AureoLightz/CitroWaker/releases"><img alt="Latest release" src="https://img.shields.io/github/v/release/AureoLightz/CitroWaker?include_prereleases&label=release&color=3a7d8c"></a>
</p>

> **CitroWaker 1.0.2.** It runs on real New Nintendo 3DS hardware. The [1.0.2 release](https://github.com/AureoLightz/CitroWaker/releases/tag/v1.0.2) includes Windows and Linux installers, a CIA and a source archive. Speed is roughly 15–25 FPS depending on the area; 30 FPS is the goal and work continues. Full-game completion is still unverified. Keep backups of `/3ds/ww3ds/user/`.

## The project
*AI DISCLAIMER* Codex and Claude Code were used for a solid chunk of this, as a college student i literally do not have the time to do it all alone.

CitroWaker brings Wind Waker's native game code to the **New Nintendo 3DS, New 3DS XL and New 2DS XL**. Built on the community decompilation, SwitchWaker and Aurora, it translates the game's GameCube graphics commands for the handheld's PICA GPU.

The goal is the original adventure with controls and presentation that belong on two screens. Previously known as **Project Windfall**.

<table>
<tr>
<td width="50%" align="center">
<img src="assets/outset-bridge.png" alt="Outset Island's rope bridge in the 3DS port, captured in Azahar" width="100%"><br>
<strong>Back to Outset</strong><br>
<sub>The island's rope bridge, running in the development build.</sub>
</td>
<td width="50%" align="center">
<img src="assets/outset-coast.png" alt="Outset Island's coast and lookout in the 3DS port, captured in Azahar" width="100%"><br>
<strong>A sea worth exploring</strong><br>
<sub>The coast and lookout at the port's native 400 × 240 output.</sub>
</td>
</tr>
</table>

<sub>Unmodified Azahar captures · Emulator images do not establish real-console rendering correctness or performance.</sub>

## Install in three steps

**You need:** a **New** 3DS / 3DS XL / 2DS XL with custom firmware and [FBI](https://github.com/Steveice10/FBI), and **your own** USA disc image of *The Wind Waker* (`GZLE01`, plain `.iso`). Nothing from the game is downloaded or included.

<table>
<tr>
<td width="55%">

1. **Download one file** for your PC from [**Releases**](https://github.com/AureoLightz/CitroWaker/releases):
   **Windows** `CitroWaker-Installer.exe` · **Linux** `CitroWaker-Installer-linux-x86_64`
2. **Run it**, choose your disc image, plug in your SD card and press **Install**. The installer includes the CitroWaker CIA and prepares the SD files from your disc image.
3. **On the 3DS**, open **FBI › SD › cia › `CitroWaker.cia` › Install CIA**, then start **CitroWaker** from the HOME Menu.

**macOS?** There is no Mac app yet, but the installer runs from Python (preinstalled with Xcode's command line tools, or from python.org). Download `citrowaker_installer.py` from the [installer folder](installer/citrowaker_installer.py) and `CitroWaker.cia` from the release into the same folder, then in Terminal: `python3 citrowaker_installer.py --iso ~/Downloads/GZLE01.iso --sd /Volumes/YOUR_SD_CARD`. Run `python3 citrowaker_installer.py` with no options to open the window instead (needs Python with Tk, e.g. the python.org build). Then do step 3.

**Updating?** Download the new installer and run it again. Your saves are kept and the disc image is not copied twice.

<sub>[Installer details & command line →](installer/README.md)</sub>

</td>
<td width="45%" align="center">
<img src="assets/installer.png" alt="The CitroWaker Installer window: disc image and SD card steps with an Install button" width="100%"><br>
<sub>The installer (Linux shown; Windows looks the same).</sub>
</td>
</tr>
</table>

The original 3DS and 2DS are not supported. Wrong-region, later-revision, trimmed or compressed (RVZ/CISO) images are rejected with instructions to fix them.

## The bottom screen

<table>
<tr>
<td width="50%" align="center">
<img src="assets/bottom-panel.png" alt="CitroWaker bottom screen: a Settings button on the left and X, Y, Z, Menu and Map touch buttons on the right" width="100%"><br>
<strong>Touch panel</strong><br>
<sub>Touch X, Y or Z to use the assigned item, Menu for the pause menu, Settings for CitroWaker's options.</sub>
</td>
<td width="50%" align="center">
<img src="assets/bottom-livemap.png" alt="CitroWaker bottom screen showing the game's live map next to the touch buttons" width="100%"><br>
<strong>Live map</strong><br>
<sub>Optional: the game's minimap drawn on the bottom screen (<i>Settings › Bottom screen</i>).</sub>
</td>
</tr>
</table>

Buttons follow the game's own HUD: usable when the game allows the action, greyed out in cutscenes and conversations. **START + SELECT** opens the settings menu anywhere; changes are saved to the SD card. [Controls →](docs/DEVELOPMENT.md#controls) &nbsp; · &nbsp; [QoL & gyro →](QOL.md) &nbsp; · &nbsp; [Touch shortcuts →](SHORTCUTS.md)

## Development status

| Area | Current state |
| :--- | :--- |
| **Game & audio** | Boots and reaches gameplay on real New 3DS; sound and physical controls run. |
| **Graphics** | Custom GX-to-PICA renderer. Some console depth, cloud and shadow faults remain under investigation. |
| **Performance** | Frame rate and game speed vary by area. CPU-side draw preparation and GX decoding both exceed the 33.33 ms frame budget in demanding tests. See the [console and emulator measurements](docs/PERFORMANCE.md). |
| **Saving** | SD-backed memory cards; the settings menu now saves on hardware. Keep backups of `/3ds/ww3ds/user/`. |
| **Touch & QoL** | Bottom-screen item buttons with icons, two touch item slots (I / II) and the live map; gyro aiming (on by default) and a Gameplay settings tab for sailing speed and QoL options. |
| **Release** | [1.0.2](https://github.com/AureoLightz/CitroWaker/releases/tag/v1.0.2) includes both installers, a CIA, checksums and a source archive. |

### Current development

- Reuse of unchanged geometry and recorded GPU commands is already in use. New prepared-command experiments preserve the captured output, but their limited coverage made them slower in live lava-room tests. They remain off by default.
- In the main Dragon Roost Cavern lava room, reusing fog shader templates reduced a heavy emulator window from 130.48 to 107.02 ms per frame. The New 3DS speedup is still unmeasured.
- The performance target is **30 presented FPS at 100% game speed**, preserving effects and rendering every frame. 1.0 fixed the Dragon Roost summit combat freeze; 1.0.1 makes the graphics thread survive running out of memory (the Gohma boss-room crash).

## Source and tools

| Component | Start here |
| :--- | :--- |
| **Installer** | [Windows/Linux GUI and command line](installer) |
| **Port source** | [3DS game layer, renderer and citro3d changes](port) |
| **3DS platform code** | [NDSP audio, touch/lifecycle logic and configuration](platform/n3ds) |
| **PC texture converter** | [Buildable extractor and converter](tools/texture-pack): process your own ISO locally. |
| **Console log reports** | [Summarize and compare timings](tools/console-report) |
| **Host tests** | `python3 tests/run.py` with Python 3.10+ and a C++17 compiler (`--python-only` for the Python tests). |

[Source scope, dependencies & licence →](docs/SOURCE.md) &nbsp; · &nbsp; [Player-release notes →](docs/PLAYER_RELEASE.md)

## Beyond Outset

The ending cutscene also boots and renders in the port. This development capture includes a measured frame-time graph beneath the game picture.

<p align="center">
  <img src="assets/ending-timing.png" alt="The ending cutscene with Link and Tetra, captured in Azahar with an emulator frame-time graph below" width="86%">
</p>

<sub>Azahar capture · The displayed timing is emulator/virtual timing, not console FPS. The Ganondorf battle has not been captured.</sub>

The [Python + FFmpeg showcase tool](tools/showcase/README.md) adds timing graphs to existing footage. It distinguishes emulator, console and host measurements and does not invent timings from the video's frame rate.

## Follow the work

| For players | For developers |
| :--- | :--- |
| [Install with the installer](#install-in-three-steps) | [Development arrangement](docs/DEVELOPMENT.md#building-from-source) |
| [QoL & gyro settings](QOL.md) | [Performance measurements](docs/PERFORMANCE.md) |
| [Touch shortcut status](SHORTCUTS.md) | [Hardware findings](docs/CONSOLE_FINDINGS.md) |
| [Report an issue](https://github.com/AureoLightz/CitroWaker/issues) | [Timing-overlay tool](tools/showcase/README.md) |

Bug reports are most useful with the build identifier, console/emulator details, scene and reproduction steps. Review diagnostics for personal information; never attach an ISO or extracted game-data pack.

---

<p align="center"><strong>Built on the community's work</strong></p>
<p align="center">
  <a href="https://github.com/zeldaret/tww">zeldaret/tww</a> &nbsp; · &nbsp;
  <a href="https://github.com/centollOS/SwitchWaker">SwitchWaker</a> &nbsp; · &nbsp;
  <a href="https://github.com/encounter/aurora">Aurora</a> &nbsp; · &nbsp;
  <a href="https://devkitpro.org/">devkitPro</a> &nbsp; · &nbsp;
  <a href="https://github.com/azahar-emu/azahar">Azahar</a>
</p>

<sub>The Legend of Zelda and The Wind Waker belong to Nintendo. CitroWaker is an independent community project, not affiliated with or endorsed by Nintendo. Screenshots contain game imagery; original project code is MIT-licensed, with no licence over Nintendo assets granted. See [repository scope and rights](RIGHTS.md).</sub>
