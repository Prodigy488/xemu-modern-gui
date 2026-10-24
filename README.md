# xemu Modern GUI: an experimental xemu fork

> [!IMPORTANT]
> **This is an experimental fork of [xemu](https://xemu.app) for trying out
> user interface improvements.** It is not official xemu, and it is not made,
> endorsed or supported by the xemu project. Some of these ideas may never
> appear in xemu.
>
> - **Please don't ask about this build in xemu's Discord, GitHub or other
>   xemu channels**, and don't report its problems there. Open an issue in
>   this repository instead.
> - If something doesn't work, try official xemu before assuming xemu has
>   the problem.
> - For the official emulator, downloads and support, go to
>   [xemu.app](https://xemu.app).

This fork is based on xemu 0.8.139. Emulation is unchanged except where noted
under Performance and stability; the work here is on the interface: a
console-style dashboard, a homebrew tab, upscaling options, tools for
finding out why a game is slow, and quality-of-life features.

> **Agent Declaration**: These changes were created with assistance from
> Claude Code (Anthropic, Claude Opus 5.5).

![Home tab, Neon theme](docs/screenshots/home.jpg)

| Library | Homebrew | Homebrew store | Drive |
|---|---|---|---|
| ![Library](docs/screenshots/library.jpg) | ![Homebrew](docs/screenshots/homebrew.jpg) | ![Store](docs/screenshots/store.jpg) | ![Drive](docs/screenshots/drive.jpg) |

| Insignia | Blades style | Metro style | Classic style |
|---|---|---|---|
| ![Insignia](docs/screenshots/insignia.jpg) | ![Blades](docs/screenshots/blades.jpg) | ![Metro](docs/screenshots/metro.jpg) | ![Classic](docs/screenshots/classic.jpg) |

| Performance Capture | Settings | Setup check |
|---|---|---|
| ![Performance Capture](docs/screenshots/perf-capture.jpg) | ![Settings](docs/screenshots/settings.jpg) | ![Setup check](docs/screenshots/setup.jpg) |

| Deutsch | 日本語 | Русский |
|---|---|---|
| ![German](docs/screenshots/lang-de.jpg) | ![Japanese](docs/screenshots/lang-ja.jpg) | ![Russian](docs/screenshots/lang-ru.jpg) |

<sub>Screenshots use original placeholder cover art (not the real box art), example compatibility ratings and invented example homebrew. The Performance Capture screenshot shows a real recording of Project Gotham Racing.</sub>

## Download

Windows builds are on the [Releases](../../releases) page. Unzip and run
`xemu.exe`. It uses the same settings folder as official xemu, so existing
settings, EEPROM and hard disk images carry over.

You still need your **own** MCPX boot ROM, BIOS (flash ROM) and hard disk
image, the same as with official xemu. They are not included and must not be
shared.

**Updating:** from v1.4 on, **Help > Check for Updates** finds new releases
of this fork (not official xemu) and installs them in place; it also checks
at startup if "Check for updates" is on. New users get a **setup check** on
the Home tab that points out any missing files.

## What's added

- **New in v1.8: languages, easy Insignia setup, voice commands.**
  - **11 languages:** English, Español, Português (Brasil), Français,
    Deutsch, Italiano, Polski, Русский, 日本語, 简体中文 and 한국어. It follows
    your Windows language; change it on the welcome screen, the dashboard's
    Settings tab or Settings > General. Japanese, Chinese and Korean use
    fonts Windows already has. Translations were made with AI help and live
    in [`ui/xui/lang`](ui/xui/lang); corrections are welcome.
  - **One-click Insignia setup:** "Set up xemu for Insignia" on the
    Insignia tab turns on networking (NAT) and Insignia's DNS. xemu answers
    the Xbox's DNS lookups with Insignia's server, so nothing has to be
    changed in the Xbox dashboard. "Test connection" checks that Insignia's
    DNS answers from your PC.
  - **Voice commands for Performance Capture (experimental, off by
    default):** "Performance capture start", "stop" and "mark", using
    Windows speech recognition on your PC.
- **New in v1.7: setup and everyday polish.**
  - **Setup check:** Home shows what's missing (MCPX boot ROM, BIOS, hard
    disk image, games folder) with a button to fix each; also in Settings
    and the welcome window.
  - **Per-game renderer and dynamic resolution:** set one game to OpenGL
    and the rest stay on Vulkan.
  - **Library filters** by compatibility rating and played / never played.
  - **New homebrew on Home**, one click to play.
  - **Performance Capture pictures:** save a summary image for posting.
  - Updater fixes, and a controller pass over the newer screens.
- **New in v1.6: performance tools.**
  - **Performance Capture:** find out why a game slows down or glitches.
    Press Shift+F9 while playing, Shift+F12 when a glitch shows, Shift+F9
    to stop. The report graphs every frame and says, in plain words, what
    changed when the game slowed down (draw calls, new shaders, images read
    back from the GPU, GPU waits, occlusion queries, uploads, or nothing
    graphical, pointing at the emulated CPU), tells real causes from things
    that only come along with them, and explains how it worked each one
    out. Saved as `report.txt`, `frames.csv` and a `trace.json` for
    [Perfetto](https://ui.perfetto.dev).
  - **Dynamic resolution (experimental):** lowers the internal resolution
    while a game runs below its usual frame rate and raises it again when
    there's room. It checks that each change helps and undoes it if not.
  - **Shaders:** a "Preparing shaders" indicator in the lower-left corner
    that fades away when done, OpenGL reusing saved shaders even after a
    crash, and **Clear shader cache** per game (right-click a game) or for
    all games (Settings tab).
  - **Drive tab:** game names next to title-ID folders in `TDATA` and
    `UDATA`.
- **New in v1.5: Homebrew tab.**
  - **Play** the homebrew on your Xbox hard disk image (`E:\Games`,
    `E:\Apps`, `E:\Emulators`) straight from the dashboard, no homebrew
    dashboard needed: xemu turns the app's folder into a disc image and boots
    it.
  - **Covers** from the store (also for apps you copied on yourself), your
    own `covers\<title ID>.png`, or the app's built-in title image.
  - **Right-click (or Y)** to delete an app or show it in the Drive tab.
  - **Get more** from [XboxHomebrew.store](https://xboxhomebrew.store) (run
    by Team Resurgent): search, browse by category, read about an app and its
    versions, then **Install to Xbox**. Games go to `E:\Games`, emulators to
    `E:\Emulators`, the rest to `E:\Apps`. Only homebrew is listed; firmware
    and Microsoft's dashboards are left out.
- **New in v1.4: compatibility ratings.** Each game shows its rating from
  xemu's community [compatibility list](https://xemu.app/#compatibility):
  a coloured dot on its cover, a badge on Home and a row in its details with
  a link to the report. Sort the library by it too.
- **New in v1.3: Blades and Metro styles.** Two more dashboard layouts:
  sliding side-by-side panels, or slim XBMC4Gamers-style lists. Save states
  also get a right-click menu (load, delete, show file).
- **New in v1.3: Xbox hard drive explorer.** A Drive tab to browse the
  emulated hard drive's C, E, F and X/Y/Z partitions and copy files and
  folders in and out, rename and delete (uses
  [libfatx](https://github.com/mborgerson/fatx)). Copying to the Xbox pauses
  it and restarts it afterwards.
- **New in v1.3: 3D game cases.** Covers on the Home screen stand as 3D game
  cases with a spine and a reflection, made from your existing covers. Use
  your own 3D box art by saving it as `covers\<title ID>-3d.png`, or switch
  back with Settings > Home screen covers. The Insignia status in the top bar
  is now color-coded (green, amber or red). Also new: **NVIDIA RTX Video (AI)**
  upscaling, using the RTX Video Super Resolution built into NVIDIA's driver
  (RTX cards, with the feature turned on in the NVIDIA app), and **NVIDIA
  Image Scaling (NIS)** for any GPU.
- **New in v1.3: motion.** The dashboard animates: flowing light ribbons, a
  slowly turning wireframe orb and drifting specks in the background (plus a
  grid floor in Neon), content that sweeps in when you switch tabs, a tab
  highlight that glides, and a selection glow. Turn it off with
  "Animations and moving background" in Settings.
- **New in v1.3: Neon theme.** A dark, cyberpunk look for the dashboard:
  near-black backgrounds, cyan and magenta accents, cut-corner panels and HUD
  labels. The previous green look stays available (Games > Library Style).
- **New in v1.1: Discord status.** While you play, your Discord profile shows
  the game you're playing, with its cover, title ID, play time and an Insignia
  badge for online-capable games. Turn it off in Settings > General > Discord
  status. Requires the Discord app running on the same PC.
- **Four library styles**, switchable any time from Games > Library Style or
  Settings: the tabbed **Dashboard**, **Blades**, **Metro**, or the
  **Classic** single library window.
- **Dashboard** with tabs, switched with LB / RB:
  - **Home:** continue your last game.
  - **Library:** your full game collection.
  - **Insignia:** online status, your online-capable games and setup steps.
  - **Media:** save states and screenshots.
  - **Drive:** the Xbox hard drive explorer.
  - **Homebrew:** play, delete or get more homebrew (XboxHomebrew.store).
  - **Patches** and **Settings.**
- **Upscaling filters:** AMD FidelityFX FSR 1.0, NVIDIA RTX Video (AI),
  NVIDIA Image Scaling (NIS),
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
  - A performance overlay (F9) and Performance Capture (Shift+F9).
  - OpenGL loads saved shaders from disk when they aren't in memory, so the
    shader cache still works after a crash or forced close.
  - Experimental dynamic resolution.
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
