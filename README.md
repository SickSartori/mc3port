<div align="center">

<img src="docs/logo.png" alt="mc3port logo" width="150">

# mc3port · Midnight Club 3: DUB Edition Remix PC Port

**An unofficial native PC port of Midnight Club 3: DUB Edition Remix for the original Xbox.**<br>
16:9 widescreen, 60 fps, up to 5K internal resolution and optional modern effects, running from your own disc image.

[![Get it on RecompLauncher](https://recomplauncher.com/assets/press/launcher/badge-dark.svg)](https://recomplauncher.com/game/mc3port)

[![Latest release](https://img.shields.io/github/v/release/SickSartori/mc3port?label=release)](https://github.com/SickSartori/mc3port/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/SickSartori/mc3port/total)](https://github.com/SickSartori/mc3port/releases)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20(Wine)-blue)](#installing)
[![Licence](https://img.shields.io/github/license/SickSartori/mc3port)](LICENSE)

[Download](https://github.com/SickSartori/mc3port/releases/latest) ·
[Features](#features) ·
[Install](#installing) ·
[Controls](#controls) ·
[Settings](#settings) ·
[FAQ](#faq) ·
[Troubleshooting](#troubleshooting) ·
[Changelog](CHANGELOG.md)

</div>

> [!IMPORTANT]
> mc3port is an unofficial, non-commercial fan project. It is not affiliated with, endorsed by or sponsored by
> Rockstar Games, Take-Two Interactive or Microsoft. Midnight Club is a trademark of Take-Two Interactive Software.
>
> **No game data is included or distributed**: no disc images, no executables, no textures, sounds, music or films.
> To play you need your own, legally obtained copy of the game. mc3port reads everything it needs from your disc
> image while it runs. Please do not ask for game files or disc images, and do not share them.

## Screenshots

![The title screen in the game's own 16:9 mode](docs/screenshots/title-16x9.jpg)

| | |
|---|---|
| ![A race in Tokyo, 16:9](docs/screenshots/race-16x9.jpg) | ![The launcher](docs/screenshots/launcher.jpg) |
| ![Launcher settings: the style above the tabs, then the Picture tab](docs/screenshots/launcher-settings.jpg) | ![Launcher controls: what each button does, rebindable keys](docs/screenshots/launcher-controls.jpg) |

<sub>The launcher speaks English or Italian, following Windows; these captures come from an Italian Windows. Its logo
and pictures are read from the player's disc. The game content in the screenshots belongs to its owners.</sub>

## Features

mc3port is a port, not a remaster: it runs the original 2005 game, with its own cars, cities, physics and
soundtrack. The game's x86 code runs natively on your PC, and mc3port replaces the Xbox around it: the kernel, the
NV2A graphics chip (on Direct3D 11), the sound chip (on XAudio2) and the controllers (on SDL3).

| | Xbox | mc3port |
|---|---|---|
| Aspect ratio | 4:3, or 16:9 with a stretched HUD | **True 16:9**: the game's widescreen mode, wider view, HUD kept round |
| Frame rate | 30 fps | **60 fps**, or 30 like the Xbox |
| Resolution | 480p | **Up to 5K**, or as many lines as your screen, with 16x anisotropic filtering |
| Picture | as shipped | **Modern effects**, all optional: glow, sharpening, colour, vignette, ambient occlusion (F10 to compare) |
| Controllers | Xbox pads | Xbox, **PlayStation, Switch** and most USB or Bluetooth pads, or the **keyboard** with rebindable keys |

And around the game:

- **A launcher** that checks your disc, sets everything up on the first start and holds every setting.
- **An in-game menu** (F1, or Back+Start on a controller) with the same settings and controls; it pauses the game.
- **A HUD of its own**, optional: speedometer, rev counter, gear and special ability drawn sharp at any resolution.
- **Sharp 2D pictures**: the radar's map, icons, letters and the cars' stickers enlarged with clean edges.
- **Your own music** on the game's radio: playlists made in the launcher's Music page from MP3, FLAC, WAV, WMA or M4A
  files, chosen in the game in Pause > Options > Audio Options.
- **Cheats** in the in-game menu: money for your career, and the game's own codes with what each one does.
- **Quality of life**: frame counter, opening films skipped, readable loading screens if you want them, an update
  check with a verified download, and a crash report that fills in the bug form for you.

## Requirements

- **Windows 10 or 11, 64-bit**, or **Linux** through Wine ([experimental](#linux-experimental)). Nothing else to
  install: the Visual C++ runtime is built in, and Direct3D 11 and XAudio2 are part of Windows. On Windows **N**
  editions, install the Media Feature Pack first.
- **A graphics card with Direct3D 11** (feature level 11_0). A fast processor core matters more than a powerful
  graphics card: see [Performance](#performance).
- **A disc image** (`.iso` or `.cci`) of **Midnight Club 3: DUB Edition Remix** for the Xbox: the **European**
  (En, Fr, De, Es, It) or the **USA** release.

<details>
<summary>Which disc images work</summary>

XISO and full Redump dumps both work, in one file or split in parts (`name.1.iso`, `name.2.iso`: choose any part),
and so does the compressed CCI format (`.cci`, also split). mc3port checks the game's executable and tells you if
the disc is a different version:

| Release | `default.xbe` SHA-256 |
|---|---|
| Europe (En, Fr, De, Es, It) | `29d866afda9c54590a854fdfa98cf6e69d871dd3fc02da20bbbf76c3582c7b09` |
| USA | `0b3d81a02834e57864a601d473d5296cc0a2463e4b837373d91030f751fa51a3` |

A copy whose executable differs only in its header or certificate (region-free or re-signed) works too. Other
games and releases (the original DUB Edition, for example) are not supported.

</details>

Optional: a controller (see [Controls](#controls)), and an internet connection for the update check only (it can
be switched off, see [Privacy](#privacy)). Without an audio device the game simply runs silent.

## Installing

| Way | How | Updates |
|---|---|---|
| **[RecompLauncher](https://recomplauncher.com/game/mc3port)** (easiest) | Find mc3port in the catalog and install it. The launcher checks your disc, keeps the main settings on the game's page and starts the game straight away. | Automatic |
| **Installer** | Download `mc3port-<version>-windows-x86-setup.zip` from the [latest release](https://github.com/SickSartori/mc3port/releases/latest), extract it and run the setup inside. It installs for your user only (no administrator rights), with Start menu and desktop shortcuts. | Offered by the launcher: it downloads the new installer, checks its SHA-256 and shows it to you |
| **Portable** | Download `mc3port-<version>-windows-x86.zip`, extract it anywhere and start `mc3port\mc3.exe`. | Extract the new version over the old one |
| **Linux (Wine)** | Download `mc3port-<version>-linux.tar.gz`: see [Linux](#linux-experimental). | Run `install.sh` again |

Every package contains `README.txt`, a short guide, and `CHANGELOG.md`. To check a download, compare its SHA-256 with
the release's `SHA256SUMS`. The files are not code signed yet (see [Code signing policy](#code-signing-policy)), so
browsers may warn about the download and Windows SmartScreen may ask once whether you trust them ("More info", then
"Run anyway"). The installer is also attached as a plain `-setup.exe`, which the launcher uses for its updates; some
browsers block that one, the zip gets through.

### First start

The launcher opens and asks for your disc image. Choose the `.iso` (or `.cci`): mc3port checks it and remembers where
it is. Pick a style, **Original** or **Enhanced**, and press **PLAY**. Next time the launcher opens on its main page,
and **PLAY** (or Enter, or A on a controller) starts the game.

### Where your files are

Settings (`mc3port.ini`), saves (`xbox_hdd`), logs (`logs`) and the shader cache live in **`Documents\mc3port`**, so
updates and uninstalling never touch them. With RecompLauncher they are in the game's own folder.

<details>
<summary>Portable data, older versions and backups</summary>

- To keep everything next to `mc3.exe` instead (a USB stick, say), create an empty file named `portable.txt` there.
- Versions up to 0.3.0 kept the files in `%LOCALAPPDATA%\mc3port`: they keep working there, and at each start the
  launcher asks whether to move them, until you do.
- Your profiles are in `xbox_hdd\Partition1\UDATA\54540079`, laid out like the Xbox's hard disk; the launcher's
  **About** page opens that folder. For a backup, copy the whole `xbox_hdd` folder.

</details>

### Linux (experimental)

mc3port is a Windows program that also runs on Linux through **Wine**. It has been tested on one machine with Wine 11
under WSL2, where the whole game worked, sound included, and a player has reported it working well through
RecompLauncher. **Not tested yet**: real Linux graphics drivers, Proton and DXVK, controllers, the Steam Deck, the
installer and the update check. If something goes wrong, please
[open an issue](https://github.com/SickSartori/mc3port/issues).

<details>
<summary>How to set it up</summary>

- **A recent Wine with 32-bit support** (tested: Wine 11.0; mc3port is a 32-bit program). On Debian and Ubuntu this
  means the `i386` architecture and the 32-bit OpenGL and Vulkan libraries (`libgl1:i386`, `libegl1:i386`,
  `libvulkan1:i386`); without them Wine has no Direct3D 11 and the window stays black. The packages from
  [WineHQ](https://gitlab.winehq.org/wine/wine/-/wikis/Download) bring the rest.
- **The Linux package**, `mc3port-<version>-linux.tar.gz`: extract it and run `sh mc3port/install.sh`. It copies
  mc3port to `~/.local/share/mc3port`, makes a Wine prefix of its own there, and adds the `mc3port` command and an
  applications menu entry. Run it again to update. Settings and saves are in that one folder; the script says how to
  remove it.
- **Or the portable zip**, by hand: extract it, put your `.iso` in the `mc3port` folder (it is found there without
  asking) or choose it in the launcher, then run `wine mc3.exe` from that folder. Your files are then in
  `~/.wine/drive_c/users/<you>/Documents/mc3port`. With Steam, "Add a non-Steam game" pointing at `mc3.exe` starts
  it through Proton (not tested yet).

</details>

## Controls

Xbox controllers (360, One and Series) work as on the console, on ports 1 to 4. PlayStation (DualShock 4, DualSense),
Switch and most other USB or Bluetooth pads work too, with the buttons in the Xbox positions (the bottom face button
is A): no Steam Input or DS4Windows needed. Without a controller, the keyboard is controller 1, and every key can be
changed in the launcher's **Controls** page.

| Key | Xbox controller |
|---|---|
| W / S | right trigger (accelerate) / left trigger (brake, reverse) |
| A / D | left stick (steer) |
| Space / Esc / Z / C | A (handbrake) / B (weight transfer) / X (nitrous) / Y (look back) |
| F | left stick click (special ability: Zone, Agro or Roar) |
| Left Shift / Left Ctrl | RB (change camera) / LB (flash headlights) |
| Enter / Backspace | Start (pause) / Back (navigation system) |
| Arrow keys | D-pad (map, hydraulics, music) |
| I / J / K / L | right stick (camera) |

| PC key | Action |
|---|---|
| F1 (controller: Back+Start) | in-game menu: settings, controls, resume, exit; the game pauses meanwhile |
| F11 or Alt+Enter | full screen on and off |
| F10 | modern effects on and off |

<details>
<summary>Moving around the launcher and the in-game menu</summary>

| Controller | Keyboard | Action |
|---|---|---|
| D-pad or left stick | arrow keys | up and down through the menu; right into the page, left back out |
| A / B | Enter / Esc | select / back |
| LB / RB | Q / E | previous and next tab (Settings) or page |
| Start | | launcher: play; in-game menu: resume |

</details>

## Settings

Everything is in the launcher's **Settings** page, in the in-game menu (F1), and in `mc3port.ini`, where each key is
explained. Language, frame rate, aspect ratio, resolution, filtering and the HUD apply the next time the game starts;
the others at once.

The quickest choice is the **style**, above the tabs:

- **Enhanced** (the default): 60 fps, 16:9, your screen's resolution, 16x texture filter, modern effects, mc3port's
  HUD, sharp pictures, no opening films, no wait on loading screens.
- **Original**: the game as on the Xbox: 30 fps, 4:3, 480p, speed blur and light streaks, the game's HUD, opening
  films, loading screens of 8 seconds.
- **Custom**: your own values, kept while you try another style. Language, volumes, window mode, speed unit and keys
  are never touched by a style.

<details>
<summary>Every setting</summary>

| Setting | What it does |
|---|---|
| Game language | English, French, German, Spanish or Italian; by default like Windows |
| Frames per second | 60, or 30 like the Xbox |
| VSync | off by default: more frames per second, but the picture can tear (a horizontal line where two frames meet). On: it never tears, but a PC that cannot keep up may drop to 30 fps |
| Aspect ratio | 16:9 (the game's widescreen mode) or 4:3 |
| Texture filter | anisotropic filtering, 1x to 16x |
| Internal resolution | Auto (as many lines as your screen; at most 1080 on integrated graphics), SD 480p (like the Xbox), HD 720p, Full HD 1080p, 2K 1440p, 4K 2160p or 5K 2880p |
| Vertices on the graphics card | on by default; switch it off only to troubleshoot |
| Modern effects | glow, sharpening, colour, vignette and ambient occlusion, on or off (F10 while playing); under them every value by itself, speed blur and light streaks too |
| Volume | the game's sound, 0 to 100; even at 100 there is room for many sounds at once without distortion |
| Frame counter | frames per second in a corner while playing; off by default |
| HUD | the game's, or mc3port's: speedometer, rev counter, gear, nitrous and special ability drawn again, sharp at any resolution, in mph or km/h (the radar stays the game's) |
| Sharp pictures | the radar's map, the icons, the letters and the cars' stickers enlarged with clean edges; on by default |
| Pause in the background | opens the in-game menu (and pauses) when you switch to another window; on by default |
| Skip the opening films | the Rockstar logos and the game's intro, straight to the title screen; on by default |
| Loading screens | how long the game waits before loading, with the screen animating: 2 seconds (off, the default), or 4, 8 or 12; any other number in `mc3port.ini` (`loading_screen_seconds`), never less than 2 |
| `hud_aspect` (ini only) | in 16:9, the HUD keeps its shape (1, the default) or is stretched as on a widescreen TV (0) |

</details>

<details>
<summary>Replacement pictures and mods</summary>

**Replacement pictures**: a PNG or JPG in a folder `textures_replace` next to `mc3port.ini`, named after a 2D
picture's hash (16 hex digits, for example `83E5DF1E4AC28289.png`), is drawn instead of the game's picture, at any
size. `MC3_DUMP_2D=1` saves the HUD's pictures with their hash in the name (see `docs/DEVELOPMENT.md`). Pictures in a
folder `launcher_backgrounds` (PNG, JPG and the other formats Windows reads) become the launcher's backgrounds
instead of the disc's artwork; the left part goes under the menu.

**Mods**: a file in a folder `mods` next to `mc3port.ini`, with the path it has on the disc (for example
`mods\flash\bg_main.xbck`), is read instead of the disc's.

mc3port includes no such pictures or files: the folders do not exist until you make them.

</details>

## Performance

The game itself is light: most of the work is turning the Xbox's graphics commands into Direct3D 11, which the
processor does mostly on one thread. **One fast processor core matters more than a powerful graphics card.** If the
game is not smooth on your PC, choose 30 fps, a lower internal resolution or the **Original** style, and please tell
us how it runs.

The first time each graphics shader is needed it is compiled, which can cause a short pause, for example on entering
a city. The results are kept in the `shader_cache` folder, so later runs do not pause for them.

## Known issues

- Only the European and USA releases of the Remix are supported.
- Online play (Xbox Live) is not available. Split screen works.
- On monitors wider than 16:9 there are thin black bars at the sides.
- The Xbox sound chip's own effects, such as reverb, are not reproduced: the sound can be drier than on the console.
- Linux (Wine, Proton) is experimental and only partly tested: see [Linux](#linux-experimental).

See the [open issues](https://github.com/SickSartori/mc3port/issues) for anything reported since.

## FAQ

**Is mc3port an emulator?**
No. An emulator imitates the whole console. mc3port runs the game's own x86 code directly on your processor and only
replaces the Xbox's hardware and system around it, which is why it can run at 60 fps in 16:9 on modest PCs.

**Does it include the game?**
No. You need your own disc image of Midnight Club 3: DUB Edition Remix for the Xbox. mc3port contains none of the
game's files.

**Can I play the PS2 or PSP version with it?**
No. mc3port works only with the Xbox Remix, European or USA release.

**Is it safe? My antivirus flagged it.**
The files are built by GitHub from this repository, and each release lists their SHA-256 in `SHA256SUMS`. Some
antivirus engines flag new, unsigned programs that load code at runtime; Microsoft reviewed `mc3runtime.dll` 0.3.4
and confirmed it is not malware. Check your download against `SHA256SUMS`, and report a flag in
[#9](https://github.com/SickSartori/mc3port/issues/9).

**Does it work on Steam Deck?**
Not tested yet. It runs on Linux through Wine, so it may: please report how it goes.

## Troubleshooting

- **"Cannot load mc3runtime.dll"**: the file must be next to `mc3.exe`. If it is there, the message says what is
  missing: on Windows N editions install the Media Feature Pack; an antivirus may also have quarantined the file
  (check it against `SHA256SUMS`).
- **The program closes at once, or the game crashes**: start it again and press **Report the crash**, or open the
  launcher's **About** page and press **Log folder**: `mc3port.log` says what happened last.
- **The game is not smooth**: choose 30 fps, a lower internal resolution, or the **Original** style.
- **Very low frame rate with MSI Afterburner or RivaTuner (RTSS) running**: add `mc3.exe` to RTSS with "Application
  detection level: None", or close them while playing.
- **No sound**: the game uses the output device that is the Windows default when it starts.
- **The sound is quiet**: check Volume in Settings (100 is the loudest), then the Windows volume.
- **The disc is refused**: only the European and USA releases of the Remix are supported; the launcher's message
  says what it found.
- **The update fails**: download the installer from the [latest release](https://github.com/SickSartori/mc3port/releases/latest)
  and run it; settings and saves are kept.

### Reporting a problem

Press **Report a problem** in the launcher's bottom bar (or in the in-game menu): it opens the form on GitHub with
your version and PC filled in, and the folder with `mc3port.log` to attach. After a crash, the next start makes
`mc3port-crash-report.zip` and offers **Report the crash**. Nothing is sent by itself; the log holds your Windows user
name in its paths. On Linux, please also give the distribution, the Wine or Proton version and the graphics driver.

## Building from source

mc3port's game-specific code is here; the shared runtime it builds on (the Xbox kernel, memory and threads, the
graphics chip's registers and textures, disc reading) is an open-source library shared with
[sm2port](https://github.com/SickSartori/sm2port): [xport](https://github.com/SickSartori/xport), a git
submodule in `external/xport`.

You need Visual Studio 2022 or newer with "Desktop development with C++" (MSVC x86 tools, Windows
SDK, CMake tools), `git submodule update --init`, then from a Developer PowerShell in the project folder:

```
cmake -S . -B build/pc-x86 -A Win32
cmake --build build/pc-x86 --config Release
ctest --test-dir build/pc-x86 -C Release
```

`pwsh -File tools/package.ps1` builds the release zip, the installer (with [Inno Setup](https://jrsoftware.org/isinfo.php)
6.6 or newer) and their `SHA256SUMS` in `dist/`. Technical notes, test switches and debugging tools are in
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) (in Italian).

## How this was made

mc3port was written with heavy use of AI (Anthropic's Claude), directed and tested by the author on real hardware.
xemu and other open-source Xbox projects were studied as references only; no code was copied from them.

## Credits

- [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut, MIT licence: the launcher's interface.
- [SDL](https://github.com/libsdl-org/SDL) by Sam Lantinga and contributors, zlib licence: the controllers.
- [Barlow](https://github.com/jpt/barlow) by Jeremy Tribby, SIL Open Font License: the launcher's font.
- The launcher's background music was generated for mc3port with ElevenLabs Music (through Magnific).
- The program icon and logo are mc3port's own (`tools/make_icon.py`), in the spirit of the game's logo.
- The launcher's logo and pictures are the game's own, read from your disc while it runs; they are not part of
  mc3port.

## Code signing policy

Free code signing provided by [SignPath.io](https://signpath.io), certificate by
[SignPath Foundation](https://signpath.org). The application is under review: until it is approved, the releases are
not signed yet.

- Committers and reviewers: [SickSartori](https://github.com/SickSartori)
- Approvers: [SickSartori](https://github.com/SickSartori)

The Windows files are built from this repository and [xport](https://github.com/SickSartori/xport) by GitHub Actions
([release workflow](.github/workflows/release.yml)); each release lists their SHA-256 in `SHA256SUMS`.

## Privacy

mc3port collects no personal data and has no telemetry. The only connection it makes on its own is the update check:
when the launcher opens, it asks `api.github.com` for this project's latest release. You can turn it off in the
installer, or at any time with `check_updates = 0` in `Documents\mc3port\mc3port.ini`; started from RecompLauncher, it
is always off. If you accept an update, the launcher downloads the new installer and `SHA256SUMS` from this project's
GitHub releases. **Report a problem** only opens GitHub's bug form in your browser, filled in for you to read and send
yourself. The game's online play is not available.

## Licence

mc3port's code in this repository is released under the [MIT licence](LICENSE), and so is the shared runtime
[xport](https://github.com/SickSartori/xport). Third-party parts keep their own licences, listed in
[THIRD_PARTY.md](THIRD_PARTY.md). The game itself, and everything read from your disc, belongs to its owners.
