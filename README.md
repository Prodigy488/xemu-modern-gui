# xemu Modern GUI (unofficial)

An **unofficial fork** of [xemu](https://xemu.app) 0.8.139, the original Xbox
emulator. It adds a console-style dashboard, better upscaling and
quality-of-life features on top of the official release.

> **Not affiliated with or supported by the xemu project.** Please don't report
> problems with this build to the xemu team; open an issue here instead. For
> the official emulator, see [xemu.app](https://xemu.app).

> **Agent Declaration**: These changes were created with assistance from
> Claude Code (Anthropic, Claude Opus 5.5).

![Home tab, Neon theme](docs/screenshots/home.jpg)

| Library | Insignia | Classic style |
|---|---|---|
| ![Library](docs/screenshots/library.jpg) | ![Insignia](docs/screenshots/insignia.jpg) | ![Classic](docs/screenshots/classic.jpg) |

<sub>Screenshots use placeholder cover art.</sub>

## Download

Windows builds are on the [Releases](../../releases) page. Unzip and run
`xemu.exe`. It uses the same settings folder as official xemu, so existing
settings, EEPROM and hard disk images carry over.

You still need your **own** MCPX boot ROM, BIOS (flash ROM) and hard disk
image, the same as with official xemu. They are not included and must not be
shared.

## What's added

- **New in v1.1: Discord status.** While you play, your Discord profile shows
  the game you're playing, with its cover, title ID, play time and an Insignia
  badge for online-capable games. Turn it off in Settings > General > Discord
  status. Requires the Discord app running on the same PC.
- **New in v1.5: 3D game cases.** Covers on the Home screen stand as 3D game
  cases with a spine and a reflection, made from your existing covers. Use
  your own 3D box art by saving it as `covers\<title ID>-3d.png`, or switch
  back with Settings > Home screen covers. The Insignia status in the top bar
  is now color-coded (green, amber or red). Also new: **NVIDIA Image Scaling
  (NIS)** as an upscaling filter.
- **New in v1.4: motion.** The dashboard animates: flowing light ribbons, a
  slowly turning wireframe orb and drifting specks in the background (plus a
  grid floor in Neon), content that sweeps in when you switch tabs, a tab
  highlight that glides, and a selection glow. Turn it off with
  "Animations and moving background" in Settings.
- **New in v1.3: Neon theme.** A dark, cyberpunk look for the dashboard:
  near-black backgrounds, cyan and magenta accents, cut-corner panels and HUD
  labels. The previous green look stays available (Games > Library Style).
- **Two library styles**, switchable any time from Games > Library Style or
  Settings: the tabbed **Dashboard** or the **Classic** single library window.
- **Dashboard** with tabs, switched with LB / RB:
  - **Home:** continue your last game.
  - **Library:** your full game collection.
  - **Insignia:** online status, your online-capable games and setup steps.
  - **Media:** save states and screenshots.
  - **Patches** and **Settings.**
- **Upscaling filters:** AMD FidelityFX FSR 1.0, NVIDIA Image Scaling (NIS),
  Lanczos, Bicubic and Snapdragon GSR 1, with a sharpening control.
- **Game library:**
  - Multiple game folders, cover art downloads and play time.
  - Favorites, per-game settings, preview videos and desktop shortcuts.
  - A "resume where you left off" option.
- **Patches:**
  - Per-game custom XBE, IPS, BPS and Magic Patch (.jmp) files, applied
    without modifying the disc image.
  - Download widescreen, 60 FPS and HD patches from
    [Jay's Magic Patches](https://github.com/JayYardley/Xbox-Magic-Patches-by-Jay).
- **Video mode switches:** widescreen, 480p, 720p and 1080i, the same as the
  Xbox dashboard's settings.
- **Insignia** (Xbox Live revival) integration: ONLINE badges, a status light
  and setup help.
- **Performance and stability:**
  - Vulkan shader and pipeline caches.
  - Higher-priority emulation threads.
  - A larger CPU code cache.
  - Windows power throttling turned off.
  - A performance overlay (F9).
  - Fixes for some Vulkan and audio crashes.

Every change is a separate commit on the `enhanced` branch, based on the
`v0.8.139` release.

## Building

The same as official xemu: `./build.sh` (or `./build.sh -p win64-cross` for
Windows). See the [xemu documentation](https://xemu.app/docs/dev/building-from-source/).

## License

xemu is licensed under the GNU GPL v2 (see `LICENSE` and `COPYING`), and so
are the changes in this fork. Patch files downloaded at runtime remain the work
of their authors.
