mc3port <version> - Midnight Club 3: DUB Edition Remix on Windows
=================================================================

An unofficial, non-commercial fan project: a native Windows port of the
European Xbox release, run from your own disc image. It is not affiliated
with, endorsed by or sponsored by Rockstar Games, Take-Two Interactive or
Microsoft. No game data is included or distributed: no disc images, no
executables, no textures, sounds, music or films. Please do not ask for
game files and do not share them.

The full guide, with pictures, is README.md in this folder and online at
https://github.com/SickSartori/mc3port


WHAT YOU NEED
-------------

- Windows 10 or 11, 64-bit. Nothing else to install: the Visual C++
  runtime is built in, and Direct3D 11, XAudio2 and the shader compiler
  are part of Windows 10 and 11.
  Windows "N" editions only: install the Media Feature Pack (Settings >
  Apps > Optional features), or mc3runtime.dll will not load.
- A graphics card and driver with Direct3D 11 (feature level 11_0).
- A disc image (.iso) of Midnight Club 3: DUB Edition Remix, Xbox, Europe
  (En, Fr, De, Es, It). XISO and full Redump dumps both work. Other
  releases (for example the North American one) are not supported: the
  launcher checks the game's executable and tells you if the disc is a
  different version.
- Optional: an Xbox controller (Xbox 360, One, Series or any XInput
  controller), up to four. Without one, the keyboard is controller 1.
  PlayStation and other controllers work through Steam Input or
  DS4Windows.
- Optional: an internet connection, only for the update check (it can be
  switched off in Settings).
- An audio output device is not required: without one the game runs
  silent.


FIRST START
-----------

1. Portable zip: extract it into a folder of your choice. Installer: run
   it; it installs for your user only, without administrator rights.
2. Start mc3.exe. The files are not code signed, so Windows SmartScreen
   may ask once: "More info", then "Run anyway". To check a download,
   compare its SHA-256 with the SHA256SUMS file of the release.
3. The launcher asks for your disc image: choose the .iso file. mc3port
   checks it and remembers where it is; if you move the file later,
   choose it again.
4. Pick a quality preset, the aspect ratio and the frame rate, then PLAY.
   Next time the launcher opens on its main page: PLAY, Enter or A starts
   the game.

The first time each graphics shader is needed it is compiled in the
background, so the first minutes can hitch; the results are kept in the
shader_cache folder and later runs are smooth from the start.


WHERE YOUR FILES ARE
--------------------

Settings (mc3port.ini), saves (the xbox_hdd folder), the log (mc3port.log)
and the shader cache live in %LOCALAPPDATA%\mc3port, normally
C:\Users\<you>\AppData\Local\mc3port. Updates and uninstalling never
touch them; to remove everything, delete that folder too.

To back up your progress, copy the xbox_hdd folder.

Portable mode: to keep everything next to mc3.exe instead (a USB stick,
say), create an empty file named portable.txt in the same folder as
mc3.exe before the first start.


UPDATING
--------

Installer: when a new version is out, the launcher says so on its first
page and offers to update. It downloads the new installer, checks its
SHA-256, installs it and reopens the game.

Portable zip: extract the new version over the old one. Settings and saves
are kept.

What changed in each version: CHANGELOG.md in this folder.


KEYS
----

Default keyboard layout (all keys can be changed in the launcher's
Controls page):

  W / S                  accelerate / brake, reverse
  A / D                  steer
  Space / Esc / Z / C    handbrake / weight transfer / nitrous / look back
  F                      special ability (Zone, Agro, Roar)
  Left Shift / Left Ctrl change camera / flash headlights
  Enter / Backspace      Start (pause) / Back (navigation system)
  Arrow keys             D-pad (map, hydraulics, music)
  I / J / K / L          right stick (camera)

  F1 (controller: Back+Start)  in-game menu: settings, controls, resume,
                               exit; the game pauses meanwhile
  F11 or Alt+Enter             full screen on and off
  F10                          modern effects on and off


IF SOMETHING GOES WRONG
-----------------------

- "Cannot load mc3runtime.dll": the file must be next to mc3.exe. If it
  is there, a Windows component is missing: on Windows N editions
  install the Media Feature Pack. An antivirus may also have quarantined
  the file; the SHA-256 in SHA256SUMS lets you check it is the original.
- The program closes at once, or the game crashes: open the launcher's
  About page and press "Log folder"; mc3port.log says what happened last.
- The game is not smooth: choose the Balanced or Original preset, or
  30 fps, in Settings. mc3port has been tested on few machines so far.
- No sound: the output device that is the Windows default when the game
  starts is the one used.
- The disc is refused: only the European release is supported; the
  launcher's message says what it found.

To report a problem, open an issue at
https://github.com/SickSartori/mc3port/issues with your Windows version,
your graphics card, what you were doing and mc3port.log attached.


KNOWN ISSUES
------------

- Only the European release is supported.
- In 16:9 the game stretches its HUD (radar, speedometer) to the wider
  screen, as it did on a widescreen TV.
- Online play (Xbox Live) is not available. Split screen works.
- On monitors wider than 16:9 there are thin black bars at the sides.
- Linux (Wine, Proton) is not supported.


LICENCE
-------

mc3port's code is released under the MIT licence (LICENSE). Third-party
parts keep their own licences: THIRD_PARTY.md and the licenses folder.
The game itself, and everything read from your disc, belongs to its
owners.
