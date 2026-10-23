# xemu Modern GUI: Classic (unofficial)

An **unofficial fork** of [xemu](https://xemu.app) 0.8.139, the original Xbox
emulator. This is the **Classic** edition: a full-window game library (grid or
list, with a details panel) instead of the tabbed dashboard, plus the same
upscaling, patch and performance features. The dashboard edition is on the
`enhanced` branch.

> **Not affiliated with or supported by the xemu project.** Please don't report
> problems with this build to the xemu team; open an issue here instead. For
> the official emulator, see [xemu.app](https://xemu.app).

> **Agent Declaration**: These changes were created with assistance from
> Claude Code (Anthropic, Claude Opus 5.5).

## Download

Windows builds are on the [Releases](../../releases) page. Unzip and run
`xemu.exe`. It uses the same settings folder as official xemu, so existing
settings, EEPROM and hard disk images carry over.

You still need your **own** MCPX boot ROM, BIOS (flash ROM) and hard disk
image, the same as with official xemu. They are not included and must not be
shared.

## What's added

- **Upscaling filters:** AMD FidelityFX FSR 1.0, Lanczos, Bicubic and
  Snapdragon GSR 1, with a sharpening control.
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
  and setup help (Games > Insignia).
- **Performance and stability:**
  - Vulkan shader and pipeline caches.
  - Higher-priority emulation threads.
  - A larger CPU code cache.
  - Windows power throttling turned off.
  - A performance overlay (F9).
  - Fixes for some Vulkan and audio crashes.

Every change is a separate commit on the `classic` branch, based on the
`v0.8.139` release.

## Building

The same as official xemu: `./build.sh` (or `./build.sh -p win64-cross` for
Windows). See the [xemu documentation](https://xemu.app/docs/dev/building-from-source/).

## License

xemu is licensed under the GNU GPL v2 (see `LICENSE` and `COPYING`), and so
are the changes in this fork. Patch files downloaded at runtime remain the work
of their authors.
