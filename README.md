# xemu Modern GUI

**An experimental, unofficial fork of [xemu](https://xemu.app) 0.8.139 with a console-style dashboard.**

> [!IMPORTANT]
> This fork is for trying out user interface ideas. It is **not** official
> xemu and is not made, endorsed or supported by the xemu project.
>
> - **Please don't ask about this build in xemu's Discord, GitHub or other
>   xemu channels.** Report problems in this repository instead.
> - If something doesn't work, try official xemu first to see whether xemu
>   itself has the problem.
> - For the official emulator, downloads and support, go to
>   [xemu.app](https://xemu.app).

> [!NOTE]
> **Vibe coded with Claude.** Every change in this fork was written by an AI,
> [Claude Code](https://claude.com/claude-code) from Anthropic, from
> plain-language requests by the repository owner, who tests the builds and
> publishes the releases. The emulator underneath is xemu's own code; the
> changes on top haven't had the review a long-running project gets. Expect
> rough edges.
>
> **Agent Declaration**: These changes were created with assistance from
> Claude Code (Anthropic, Claude Sonnet 5.5; earlier versions Claude Opus 5.5).

Emulation is unchanged except where noted under
[Performance and stability](#performance-tools-and-stability). The work is on
the interface: a dashboard in 11 languages that works fully with a controller,
one-click 60 FPS patches, upscaling and CRT filters, a homebrew tab and store,
save backups, tools for finding out why a game is slow, a guided Insignia
setup, per-game keyboard and mouse setups and an experimental achievements
tab (v2.1), four more dashboard styles, achievements that can unlock from the
game's memory and stick drift tools (v2.2), performance settings in v2.3 (CPU
boost, GPU boost and per-game settings with an automatic safe mode, plus a fix
that lets NFL Fever 2004 run), and, new in v2.4, a faster game library with a
"Clear library" button and a 1.4 GHz CPU option.

## Highlights

- **Console-style dashboard** with Home, Library, Insignia, Media, Drive,
  Homebrew, Patches and Settings tabs, eight styles (v2.2 adds Original Xbox,
  Original Blades, Glossy Blades and Modern) and two themes. Everything works
  from a controller.
- **Game library** with cover art, play time, per-game settings, preview
  videos, save states and resume.
- **One-click 60 FPS, widescreen and HD patches** without touching your disc
  images.
- **Keyboard, mouse and controller together** as a single input choice, with
  per-game keys and mouse buttons (v2.1).
- **Picture quality:** FSR, NVIDIA RTX Video, NIS, Snapdragon GSR, Lanczos,
  Bicubic, CRT TV and scanline filters.
- **Homebrew tab and store**, plus an Xbox hard drive explorer and save
  backups.
- **Guided Insignia setup** for online play.
- **Performance Capture** to find out why a game slows down.
- **Achievements (experimental, work in progress):** fan-made packs you tick
  off yourself (v2.1) or that unlock from the game's memory (v2.2).
- **Stick drift test, a dead zone for each stick, pause when the window loses
  focus and a bug-report diagnostics button** (v2.2).
- **CPU boost and GPU boost, per-game settings and safe mode** (v2.3), and a
  floating-point fix that lets NFL Fever 2004 run.
- **Faster library scanning and Clear library** (v2.4): games appear as they
  are found, and Library > Folders > Clear library empties the list so you can
  add your folders again. The version is shown in the window title.

![Home tab, Neon theme](docs/screenshots/home.jpg)

| Library | Homebrew | Homebrew store | Drive |
|---|---|---|---|
| ![Library](docs/screenshots/library.jpg) | ![Homebrew](docs/screenshots/homebrew.jpg) | ![Store](docs/screenshots/store.jpg) | ![Drive](docs/screenshots/drive.jpg) |

| Insignia | Blades style | Metro style | Classic style |
|---|---|---|---|
| ![Insignia](docs/screenshots/insignia.jpg) | ![Blades](docs/screenshots/blades.jpg) | ![Metro](docs/screenshots/metro.jpg) | ![Classic](docs/screenshots/classic.jpg) |

| On-screen keyboard | Performance Capture | Settings | Setup check |
|---|---|---|---|
| ![On-screen keyboard](docs/screenshots/keyboard.jpg) | ![Performance Capture](docs/screenshots/perf-capture.jpg) | ![Settings](docs/screenshots/settings.jpg) | ![Setup check](docs/screenshots/setup.jpg) |

| 60 FPS games | Game saves | Achievements |
|---|---|---|
| ![60 FPS games](docs/screenshots/sixty-fps.jpg) | ![Game saves](docs/screenshots/game-saves.jpg) | ![Achievements](docs/screenshots/achievements.jpg) |

| Deutsch | 日本語 | Русский |
|---|---|---|
| ![German](docs/screenshots/lang-de.jpg) | ![Japanese](docs/screenshots/lang-ja.jpg) | ![Russian](docs/screenshots/lang-ru.jpg) |

<sub>Screenshots use original placeholder cover art (not the real box art), example compatibility ratings, invented example homebrew and example achievements and progress. The Performance Capture screenshot shows a real recording of Project Gotham Racing.</sub>

## Download

Windows builds are on the [Releases](../../releases) page. Unzip and run
`xemu.exe`. It uses the same settings folder as official xemu, so existing
settings, EEPROM and hard disk images carry over.

A Linux x86_64 AppImage (and a tarball) is also on the Releases page, for
testing: make the AppImage executable and run it. It has had very little
testing and needs glibc 2.35 or newer (Ubuntu 22.04+, Fedora 36+, Debian 12+).

You still need your **own** MCPX boot ROM, BIOS (flash ROM) and hard disk
image, the same as with official xemu. They are not included and must not be
shared.

Windows is the main platform. The Linux AppImage (new in v2.3, updated in v2.4) is lightly
tested; macOS builds from source are untested.

**Updating:** from v1.4 on, **Help > Check for Updates** finds new releases
of this fork (not official xemu) and installs them in place; it also checks
at startup if "Check for updates" is on. New users get a **setup check** on
the Home tab that points out any missing files.

## What's added

A summary of everything this fork adds. For what changed in each version,
see the [Releases](../../releases) page.

### A console-style dashboard

- **Eight styles**, switchable any time: the tabbed **Dashboard**, **Blades**
  (sliding side-by-side panels), **Metro** (slim XBMC4Gamers-style lists), **Original Xbox** (a from-scratch homage to the first Xbox's menu with a glowing wireframe globe; no Microsoft artwork), **Original Blades** (a bright green web, a glowing sphere with fins and an arc of orbs, one per section), **Glossy Blades** (silver blades on glossy green, in the spirit of the Xbox 360's first dashboard) and **Modern** (a side menu with icons, and one calm card)
  and the **Classic** single library window.
- **Two themes:** **Neon** (near-black with cyan and magenta, cut-corner
  panels and HUD labels) and **Green** (the rounded look in xemu green).
- **Motion:** flowing light ribbons, a turning wireframe orb, drifting
  specks, tab transitions and a gliding selection glow. Can be turned off.
- **Tabs**, switched with LB / RB: **Home** (continue your last game),
  **Library**, **Insignia**, **Media** (save states and screenshots),
  **Drive**, **Homebrew**, **Patches** and **Settings**.
- **Built for controllers:** everything works from a gamepad, with button
  hints along the bottom.
- **On-screen keyboard:** select any text box with a controller and a
  keyboard in the dashboard's colors slides up. D-pad to pick a key, A to
  type, X to delete, Y for a space, LT for symbols, RT for capitals, B to
  close. Can be turned off in Settings.
- **Minimize:** tuck the dashboard away to a small tab at the bottom of the
  screen and bring it back with one click, Ctrl+L or the Guide button menu.
- **11 languages:** English, Español, Português (Brasil), Français,
  Deutsch, Italiano, Polski, Русский, 日本語, 简体中文 and 한국어. It follows
  your Windows language; change it on the welcome screen or in Settings.
  Translations were made with AI help and live in
  [`ui/xui/lang`](ui/xui/lang); corrections are welcome.
- **Setup check:** new users see what's missing (MCPX boot ROM, BIOS, hard
  disk image, games folder) with a button to fix each.
- **Updates built in:** Help > Check for Updates installs new releases of
  this fork in place.

### Your game library

- **Multiple game folders**, cover art downloads, play time, favorites,
  filters (compatibility, played / never played) and sorting.
- **3D game cases** on Home, made from your covers, or your own 3D box art
  (`covers\<title ID>-3d.png`).
- **Compatibility ratings** from xemu's community
  [compatibility list](https://xemu.app/#compatibility) on every game.
- **Per-game settings:** resolution, upscaling filter, sharpening and
  renderer (one game on OpenGL, the rest on Vulkan).
- **Preview videos**, **desktop shortcuts**, a **resume where you left
  off** option and **save states** with thumbnails and a right-click menu.
- **Discord status** showing the game you're playing, with its cover and
  play time.
- **Quick save and load from the controller:** Back + LB saves the game
  you're playing, Back + RB loads it (one quick save per game).

### Patches

- Per-game XBE, IPS, BPS and Magic Patch (.jmp) files, applied without
  modifying the disc image.
- Download widescreen, 60 FPS and HD patches from
  [Jay's Magic Patches](https://github.com/JayYardley/Xbox-Magic-Patches-by-Jay)
  for your whole library at once.
- **One-click 60 FPS:** games with a community 60 FPS patch (over 70 games)
  get a badge and a switch; xemu downloads the patch if needed and turns
  on just its 60 FPS options.

### Homebrew and the Xbox hard drive

- **Homebrew tab:** play the homebrew on your Xbox hard disk image
  (`E:\Games`, `E:\Apps`, `E:\Emulators`) straight from the dashboard; xemu
  turns the app's folder into a disc image and boots it. Covers come from
  the store, your own files or the app itself.
- **Homebrew store:** browse [XboxHomebrew.store](https://xboxhomebrew.store)
  (run by Team Resurgent), read about an app and **Install to Xbox** in one
  click. Only homebrew is listed.
- **Drive tab:** an Xbox hard drive explorer for the C, E, F and X/Y/Z
  partitions. Copy files and folders in and out, rename and delete, with
  game names shown next to save folders (uses
  [libfatx](https://github.com/mborgerson/fatx)).
- **Game save backups:** copy the Xbox's saves (`E:\UDATA`, `E:\TDATA`) to
  a dated folder on your PC and put a backup back in one click.
- **Safe save states:** a save state puts the whole Xbox back, hard drive
  included, so loading one made before you installed homebrew or changed
  the drive would undo that. xemu remembers those changes and asks first,
  with "Start the game without it" to keep the drive as it is.

### Online play with Insignia

- **ONLINE badges** on supported games, a status light in the top bar and
  an Insignia tab listing your online-capable games.
- **One-click setup:** "Set up xemu for Insignia" turns on networking and
  answers the Xbox's DNS lookups with Insignia's server, so nothing has to
  be changed in the Xbox dashboard. "Test connection" checks it from your
  PC.
- **Account walkthrough:** step by step from networking to playing online.
  One button downloads Insignia's Setup Assistant from their GitHub page,
  installs it on the Xbox drive and starts it; closing it brings you back
  to the next step. Then get an invite code and create your Xbox Live
  account in the Xbox dashboard.

### Picture quality

- **Upscaling filters:** AMD FidelityFX FSR 1.0, **NVIDIA RTX Video (AI)**
  (RTX cards), NVIDIA Image Scaling (NIS), Snapdragon GSR 1, Lanczos and
  Bicubic, with a sharpening control.
- **CRT TV and Scanlines:** the look of an old TV (scanlines that follow
  the Xbox's own lines, an aperture-grille mask and curved glass), with a
  strength slider, and an **Integer (pixel-perfect)** display mode.
- **Video mode switches:** widescreen, 480p, 720p and 1080i, the same as
  the Xbox dashboard's settings.

### Achievements (experimental, work in progress)

> [!WARNING]
> Achievements are **experimental and still being worked on**. Expect missing
> features, rough edges and changes between versions, including to the pack
> format. Progress is kept in `achievements.json`; keep a copy of any packs you
> write.

- **Achievements for each game.** The original Xbox had none (Gamerscore
  came with the Xbox 360), and no fan-made collection turned up either, so
  nothing is built in. They come from **achievement packs**: small JSON files
  named after a game's title ID (for example `4D530004.json`) in the
  `achievements` folder next to your library. Anyone can write and share
  one. The format is in [docs/achievement-packs.md](docs/achievement-packs.md),
  with a starting file in
  [docs/achievement-pack-example.json](docs/achievement-pack-example.json).
- **You tick them off yourself.** xemu can't see inside a game, so it can't
  tell when you've done something. A pop-up slides in at the lower right when
  you tick one (it can be switched off, and "Show a pop-up" tries it).
- **Unlock by itself (new, experimental):** an achievement can have a
  *trigger*: conditions about numbers in the running game's memory, like
  "lives is 0" or "level is at least 5". xemu checks them while the game
  runs and unlocks the achievement when they hold. The **Address finder**
  (button in the Achievements tab) helps you find those numbers, like a
  cheat search: look for a value, change it in the game, look again. It only
  reads memory, never changes it. Triggers need someone to find the addresses
  game by game, so only games people have set up unlock this way; see
  [docs/achievement-packs.md](docs/achievement-packs.md).
- **The Achievements tab** has a game picker, a progress bar with points and
  the list for the game (grouped, filterable, with secret ones hidden until
  you get them). Games without a pack are listed too.
- **Write your own:** "Add an achievement" makes one for the selected game,
  and you can edit or delete it later. Yours are kept in
  `<TITLEID>-mine.json`, next to the packs.
- **Community packs:** "Browse community packs..." lists packs other people
  have shared in this repository's [community-packs](community-packs/) folder
  and downloads the ones you pick. Each file is size-limited and checked
  before it is saved, and only shown as text. To share yours, open a pull
  request (the folder's README explains how). The list is empty so far.
- **RetroAchievements sign-in:** you can sign in with a
  [RetroAchievements](https://retroachievements.org) account. It has no
  original Xbox games yet, so for now this only keeps your account ready for
  when that changes. Your password is never saved, only the token
  RetroAchievements sends back.

### Controllers, keyboard and mouse

- **"Keyboard, mouse and controller" input choice:** in Settings > Input (or
  the dashboard's Settings tab, which has the same pickers for ports 1 to 4),
  pick it for a port and that port hears the keyboard, the mouse and every
  connected controller at once. Nothing to switch and no button to press
  first. The mouse is captured by itself while you play (F3 lets go, a click
  takes it back) and can be turned off.
- **Mouse:** moves the right stick (or the left, your choice) with its own
  sensitivity. Choose what each mouse button, the side buttons and the wheel
  do (any Xbox button or trigger; by default left click is the right trigger,
  right click the left trigger and middle click R3). **Use WASD + mouse
  layout** binds a ready-made keyboard layout in one click.
- **Test your controller:** the dashboard's Settings tab draws a controller
  that lights up as you press buttons and move the sticks and triggers, to
  check that it reaches xemu. Press **Start test** first: while it runs, the
  controller doesn't scroll the page, go back or change tabs. Stop it with
  the button, Esc, or by holding Back + Start for 2 seconds.
- **Resuming keeps your controllers:** when you resume a game or load a saved
  state from the library, your controllers are plugged in again so the game
  doesn't sit on a "connect a controller" message.
- **Keys and mouse for each game:** in a game's settings, "Own keyboard and
  mouse setup for this game" gives it its own keys, mouse speed and mouse
  buttons, switched on when the game starts and put back when it's done.
  Start from a preset by kind of game (shooter or action, racing, fighting,
  sports and platformers) or xemu's own keys, then change any key. The
  presets are sensible starting points, not each game's own layout;
  "Suggested for this game" picks one by the game's name, as a guess only.
- **On-screen keyboard capitals:** hold RT (R2) for capitals and LT (L2) for
  symbols.
- **Quick save buttons for modern controllers:** quick save and load were
  Back + LB/RB, and modern controllers have no button called Back (it's View
  on Xbox One and Series, Share on PS4, Create on PS5). Choose what you press
  while holding that button: LB/RB, the triggers, stick clicks or D-pad
  left/right. The hints show the right button names for your controller.
- **Rumble strength** for every controller, from full to off.
- **Stick dead zone and drift test:** each stick has its own dead zone (0 to
  40%) that ignores small movements around the centre, for sticks that
  drift. **Check for stick drift** (the dashboard's Settings tab, under the
  controller test, or Settings > Input) watches both sticks for 3 seconds
  while you leave them alone, says how far each rests from the centre and
  can set a dead zone for each. Physical controllers only; off by default.
- **Pause when the window loses focus (optional):** pauses the game when you
  switch to another window and carries on when you come back. A pause you
  made yourself stays. Off by default.

### Sound

- **Output device:** send the game's sound to headphones, speakers or the
  TV without changing the Windows default.
- **Background audio:** keep playing, play quieter or mute when xemu isn't
  the active window.
- **Per-game volume** for games that are much louder or quieter than the
  rest.

### Performance tools and stability

- **Performance Capture:** find out why a game slows down or glitches.
  Press Shift+F9 to record and Shift+F12 to mark a glitch. The report
  graphs every frame and explains in plain words what changed when the
  game slowed down, with a summary picture for sharing and a trace for
  [Perfetto](https://ui.perfetto.dev). Optional **voice commands**
  ("Performance capture start / stop / mark") using Windows speech
  recognition.
- **Performance overlay** (F9) and a "Preparing shaders" indicator.
- **Copy diagnostics for a bug report:** Help > Copy Diagnostics for a Bug
  Report puts your xemu version, graphics card, main settings and the game's
  title ID on the clipboard, to paste into an issue. It has no file paths,
  names or accounts. It also counts **frame hitches** (frames that came late)
  and how many were spent building shaders or pipelines, so a stutter report
  says where the stutter came from. Late frames are also noted in `xemu.log`.
- **Dynamic resolution (experimental):** lowers the internal resolution
  when a game runs slow and raises it again when there's room.
- **Shader caches:** Vulkan shader and pipeline caches, OpenGL reusing
  saved shaders even after a crash, and **Clear shader cache** per game or
  for all games. The Vulkan pipeline cache is now saved on a background
  thread, so the periodic save no longer freezes the picture.
- **Prefer the fastest CPU cores (optional):** on CPUs with performance and
  efficiency cores, keeps the emulated CPU, GPU and audio on the fast cores
  (Settings > General > Performance, restart needed). Off by default.
- **CPU boost and GPU boost (v2.3):** two settings in Settings > General >
  Performance. CPU boost runs the game's SSE math on your PC's own processor
  and trims work in the CPU emulation; GPU boost turns on faster paths in the
  graphics code (such as skipping textures a shader never reads). Their ideas
  and some of their code come from Tovarichtch's xemu fork
  (<https://github.com/Tovarichtch/xemu>, branch `perf-experimental`). Both
  can be off if a game misbehaves.
- **Per-game settings and safe mode (v2.3):** each game can have its own CPU
  boost, GPU boost and shader seeding, applied the next time it starts
  (Settings > General > Performance > This game). If xemu closes
  unexpectedly while a boost was on, that game starts in safe mode next time
  with the boosts off, and tells you.
- **Fewer game freezes and a hitch monitor (v2.3):** graphics interrupts
  reach the game right away (this fixed UFC: Tapout 2 stopping), and a
  hitch monitor says in `xemu.log` whether a late frame came from shaders,
  disc reads, code translation or the interface.
- **NFL Fever 2004 runs (v2.3):** the emulated floating-point unit now keeps
  full precision when a game loads a number while set to single precision,
  from the xemu project's pull request #2951 (by Korze Warrior). That mode is
  only used for games that need it, because it slows others down, so the
  game gets it automatically and every other game keeps the faster path.
- **Hardware virtualization (experimental, Windows):** runs the emulated CPU
  on the Windows Hypervisor Platform. Off by default; some games may not
  work with it.
- **Debug mode:** start xemu with the environment variable `XEMU_DEBUG_MODE=1`
  to get detailed timing and crash reports in `xemu.log`. For bug reports.
- **Faster library scanning (v2.4):** the library walk hands each disc image
  to several readers as soon as it is found, so games appear while the scan
  is still running, and folders holding `default.xbe` (extracted games) are
  not searched file by file. **Clear library** (Library > Folders) empties the
  game list, game folders, individually added games and hidden games so you
  can start again; game files, play time, favorites, names and per-game
  settings are not touched.
- **1.4 GHz CPU option (v2.4):** Real hardware speed can pace the CPU to 733
  MHz (stock) or 1.4 GHz, like a modded console (Settings > Performance). Real
  hardware speed is off by default. Also new: per-game display refresh,
  a hover description on every setting, more on-screen keyboard layouts and
  renaming games.
- **Upstream xemu fixes merged (v2.3):** R6G5B5 textures, NV2A memory marked
  dirty on blits and surface downloads, rgba swizzles in generated shaders
  and VulkanMemoryAllocator 3.5.0.
- **Under the hood:** higher-priority emulation threads, a larger CPU code
  cache, Windows power throttling turned off, and fixes for some Vulkan and
  audio crashes. These are the only changes to emulation.

The changes live on the `enhanced` branch, based on the `v0.8.139` release.

## Building

The same as official xemu: `./build.sh` (or `./build.sh -p win64-cross` for
Windows). See the [xemu documentation](https://xemu.app/docs/dev/building-from-source/).

## License

xemu is licensed under the GNU GPL v2 (see `LICENSE` and `COPYING`), and so
are the changes in this fork. Patch files downloaded at runtime remain the work
of their authors.
