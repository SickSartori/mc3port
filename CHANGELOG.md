# Changelog

## 0.3.3 (2026-11-08)

### Improved

- "Report a problem" fills in more of the GitHub form: how mc3port is installed next to the system and, after a crash,
  the crash itself in a few lines, so a report says where the game stopped even without the log attached.

### Fixed

- **A crash in a race when the music changes** (back to the dashboard): the 0.3.2 fix for the pedestrians also
  changed how the music ends, and the music's decoder then wrote past its buffer. The fix now applies to short sounds
  only.

## 0.3.2 (2026-11-08)

### New

- **Your own launcher backgrounds**: pictures (PNG, JPG and the other formats Windows reads) in a folder
  `launcher_backgrounds`, in the data folder, are shown instead of the disc's artwork.

### Improved

- mc3port's HUD takes the colour chosen in the garage's paint shop, the custom one too, instead of always orange.
- In the launcher, A no longer changes a setting's value: left and right do, with the D-pad, the arrows or the stick.

### Fixed

- **Stuck on "Checking saved games"** on fast PCs: a wait in the game's own graphics code compared two counters in a way
  that could never end once the graphics card ran ahead; it now ends. If the game still stops there, the log says where.
- Your own launcher backgrounds keep more of their colour than the disc's artwork.
- **Pedestrians** can be heard again: short sounds played from the disc's sound banks were dropped before they played.
  They are also a little louder, closer to the other sounds.
- **Loading screens**: the time chosen now keeps them on screen, still animating; before, the game had already gone on.
  The shortest, Off, is now 2 seconds, enough to see the screen.

## 0.3.1 (2026-11-06)

### New

- **RecompLauncher**: `recomplauncher.json` in the release, so the launcher checks your disc, keeps the settings on
  the game's page and starts the game straight away. Started from it, mc3port leaves updates to the launcher, opens
  full screen in Big Picture mode, and exits with code 2 when the disc given is refused.
- The game's button symbols say **LB and RB** where the original controller had the White and Black buttons.
- **Crash report**: the first start after a crash keeps that crash's log, which it used to overwrite, with the crash
  dump in `mc3port-crash-report.zip`, and offers **Report the crash** on the launcher's first page and in the bottom
  bars: it opens the bug report form and shows the file to attach. Nothing is sent by itself.
- **Mods**: a file in the `mods` folder, in the data folder, with the path it has on the disc (`mods\flash\...`), is read
  instead of the disc's.
- Resolution up to **5K (2880p)**.
- A replacement picture of the game's logo in `textures_replace` is shown in the launcher too.
- **Save folder**: the launcher's About page opens the folder with your profiles.
- If the graphics card stops responding, mc3port says so and starts again, once.

### Improved

- Disc images whose `default.xbe` differs only in its header or certificate (region-free or re-signed) are accepted:
  the check also compares the executable's code.
- Sound effects are resampled with a better interpolation: less of the false high end they had, above all on the wind
  and the engine at full speed.
- The cars' stickers no longer show steps along their edges at high resolutions: they are enlarged like the HUD's
  pictures, under the same setting, now called **Sharp pictures**.
- The log says when RivaTuner Statistics Server (RTSS) is hooked into mc3port, which can slow it down a lot.
- The music keeps more of its top octave: streams are resampled like the effects.
- The log says where the game's threads are when no new frame comes for 8 seconds, without any setting.
- Times shown by the game follow the time zone of Windows.
- **Loading screens**: off, 4, 8 or 12 seconds, off by default, also on RecompLauncher's page; `mc3port.ini` takes any number.
- The logs of the two runs before are kept, as `mc3port.1.log` and `mc3port.2.log`: starting the game again no longer
  loses the log of a run that went wrong. Logs and crash reports are now in the data folder's `logs` folder.
- **Your files are easy to find**: saves, settings, logs and mods now go in `Documents\mc3port`. If you have played an
  earlier version, your files keep working where they are, and the launcher asks at each start whether to move them.
- **Style**: one choice above the settings' tabs, **Original** (the game as on the Xbox), **Enhanced** (the best this
  port can show) or **Custom** (your own values, kept when you try another style). It replaces Quality and Effects
  style, and the first-run guide asks for it. New installations start in Enhanced, so at 60 fps with mc3port's HUD;
  existing settings stay as they are.
- The settings' tabs are now Picture, Effects, Game and Audio, and Controls has two: game keys and PC keys. Small
  texts are bigger and the launcher's background pictures softer.
- With a controller or the keyboard, the launcher now lights the whole selected row, and left and right change its value
  at once. On a page with a big button, that button is selected first, such as Back to game in the in-game menu.
- The program's folder holds only `mc3.exe`, `mc3runtime.dll` and `README.txt`, with the other documents in `docs`.
- If RivaTuner Statistics Server (RTSS) is hooked into mc3port, the launcher and the in-game menu say so and how to
  turn it off for the game.
- Lighter on the processor while driving: in a free drive at 60 fps the game's core went from 39% to 28% busy, and the
  emulated graphics chip's thread does about a fifth less work per draw. The frame rate on a fast PC stays at 60; the
  room is for slower ones.
- No more pause of about a third of a second on entering a city at every start: the shaders made from the game's
  materials are now kept in `shader_cache` too, so only the first run compiles them.

### Fixed

- **The cars' engines could hardly be heard and did not follow the revs**: the sounds of a car, of the wind and of other
  traffic play through a shared voice that sets their distance, pan and, for an engine, the revs; mc3port ignored it and
  played them at full level at the pitch of their recording. Voices also keep the 6 dB of headroom they start with. Far
  cars and the wind no longer cover your engine, and the overall level was raised to match. These sounds are also
  centred and stronger next to the music.
- The nitrous gauge stayed behind in 16:9 and, with mc3port's HUD, floated between the rev counter and the gear: it now
  follows the game's dials, and mc3port's HUD draws it as an arc above the speedometer, in the colour chosen in the shop.
- In the garage's menus a press of the d-pad or the left stick is one step, and holding repeats at a menu's pace.
- With a controller, A did nothing on a page just entered from the menu until a direction was pressed. On the in-game
  menu's Exit page, with its one button, the controller could not exit.
- In 16:9 a strip of the radar's map could show outside its ring: the game's mask did not follow the narrowed radar.
- Sound effects were cut short: the game gives nearly every effect a fade-out of a third of a second and starts
  them looping, releasing them at once so that they play through and fade; mc3port read the fade-out from the wrong
  envelope and faded each effect out 5 ms after it started. The engine, wind, tyre and crash sounds now play whole.

## 0.3.0 (2026-11-04)

### New

- **Crash dump**: the first crash of a run writes `mc3port-crash.dmp` next to `mc3port.log`, to attach to a report
  with the log. It holds no file paths.
- **Linux package** (Wine): `mc3port-<version>-linux.tar.gz`, with `install.sh`: it copies mc3port to your user
  folder, makes a Wine prefix of its own there, and adds the `mc3port` command and an entry in the applications menu.
  Run it again to update; settings and saves are in the same folder.
- **Cheats** in the in-game menu: with a career loaded, its money and a button that adds $10,000; and the game's own
  cheat codes, with what each does in this version, to type in its Options > Cheat Codes.
- **Loading screens** stay up at least 6 seconds, long enough to read who you race and for what (from a disc image
  the loading is over at once). Settings > Game: off, 2, 4 or 6 seconds.

### Improved

- Faster drawing: Direct3D's state is set only when it changes and textures are checked for changes more quickly
  (in races on the test PC, about 11% more frames).
- mc3port's HUD is 15% smaller.

- Fewer antivirus false alarms: `mc3.exe` no longer has a section shaped like a packed program's (on VirusTotal, 3
  engines instead of 9). The installer no longer has a script section nor starts the game by itself after a silent
  update (3 engines instead of 6, Microsoft among those gone): after an update started by a launcher of 0.2.4 or
  earlier, start mc3port as usual.
- mc3port's HUD: the special ability's arc pulses when it is full and the ability can be used.

### Fixed

- A crash in the city after some minutes of driving, in the game's sound code (XACT), then back to the dashboard:
  memory the game gives back keeps its contents, as on the Xbox, until the game asks for it again.
- XACT's stream callbacks run when a stream is flushed or released, as on the Xbox, instead of later.
- The game's threads could run while the interrupt was at DISPATCH_LEVEL (something the Xbox never allows): during
  heap allocations and when Windows raised a thread's priority, for example while the game window is in front.
- mc3port's HUD showed the special ability as empty once it was more than a third charged.

## 0.2.6 (2026-11-04)

### New

- **Split and compressed disc images**: an image split in parts, as for the Xbox's own hard disk (`name.1.iso`,
  `name.2.iso`; choose any part), and the compressed CCI format (`.cci`, also split) work like a single `.iso`.
- **Report a problem**, in the bar at the bottom of the launcher and the in-game menu (and on the About page): opens
  the bug report form on GitHub with the version and the PC filled in, and the folder with `mc3port.log` to attach.

### Improved

- A newer version shows in the same bar too, next to the version number ("Update to ..."), and leads to the About
  page that installs it.
- A row's description in the launcher stops before its button instead of running under it.
- mc3port's HUD drops the three small bars left of the speedometer (the game's light for the rev limiter, which
  the rev counter's red zone already shows) and draws the special ability's arc only once it holds a charge.
- The log says which part of showing a frame took the time when it took more than a quarter of a second.

## 0.2.5 (2026-11-04)

### New

- **Internal resolution by name**: Auto (as many lines as your screen, the default), SD 480p (like the Xbox), HD 720p,
  Full HD 1080p, 2K 1440p and 4K 2160p, instead of 1x to 4x. 1080p and 4K are not whole multiples of the game's 480
  lines; the renderer now handles that. An earlier 2x becomes Full HD, 4x becomes 4K.
- **Effects style**: Original (the Xbox's picture, speed blur and light streaks included), Enhanced (the modern
  effects, without the blur and the streaks; the default) or Custom, which alone shows every value, in two columns
  that stay on screen. The speed blur and the streaks from street lights are now separate settings, both off by
  default; an untouched 0.5 from earlier versions becomes off.
- **The HUD keeps its shape in 16:9**: the game drew it with 4:3 sizes stretched to the wider screen, so the radar
  and the dials were oval. They are round again, in their corners, with the radar's map and icons and the special
  ability's arc; texts and the rest of the picture are untouched. `hud_aspect = 0` in `mc3port.ini` brings back the stretched HUD.
- **mc3port's HUD** (Settings, Display; off by default): the speedometer, the rev counter, the gear and the special
  ability's charge drawn again by mc3port, sharp at any resolution, in mph or km/h. The values come from the game's
  own dials, which are then left out; the radar stays the game's.
- **Sharp HUD pictures** (Settings, Graphics; on by default): the radar's map, the icons and the letters, small
  pictures the game stretches over many pixels, are enlarged with their edges kept instead of blurred.
- **Replacement pictures**: a PNG or JPG named after a 2D picture's hash in a folder `textures_replace` next to
  `mc3port.ini` is drawn instead of it, at any size. `MC3_DUMP_2D=1` saves the HUD's pictures with their names.
- **Skip the opening films** (Settings, Game; on by default): straight from PLAY to the title screen, without the
  Rockstar logos and the game's intro.
- **Restart from the in-game menu**: after changing a setting that applies the next time the game starts
  (language, frame rate, aspect ratio, resolution, filtering), the menu offers to restart the game at once.

### Improved

- The Quality preset no longer sets the resolution, so choosing one never undoes the other, and the chosen preset
  stays selected.
- A setting's description no longer runs under a wider control next to it.
- A little more of the picture's edge is left out, as a television's overscan did: 1/48 of each side instead of
  1/80. The game's notification box at the top left now meets the edge of the screen.

- **Update** no longer installs the new version by itself: the launcher downloads the installer, checks its SHA-256
  and shows it in Explorer, and you start it. Some antivirus programs took a program that starts a downloaded
  installer on its own for malware. The installer is saved only once its check passes.

### Fixed

- Choosing Full screen in the in-game menu (or the launcher) made the window stop answering, and Windows then closed
  the game. F11 and Alt+Enter were not affected.
- Crashes at random moments in the career, in the game's sound code, and while loading a race: 0.2.3 made them
  rarer, but they were still there. On the Xbox nothing else runs while the system is in the middle of certain
  work (interrupts, sound updates). When that work needed something another part of mc3port was using at that
  moment (the sound mixer, the timer, the graphics chip's registers), it waited for it, and Windows ran the rest of
  the game in the meantime. Now it waits without letting go of the processor, as on the Xbox.
- The log no longer gets a line each time the game switches the format it draws into, several times per frame: it
  is about a hundred times smaller, and writing it no longer holds up the game.

## 0.2.4 (2026-11-01)

### New

- **Linux, through Wine (experimental)**: the README has a Linux section with what it takes (a recent Wine with
  32-bit support, the portable zip) and where the files are. Tested on one machine only, with Wine 11 under WSL2:
  the launcher, the opening films and the career's menus work, sound included. Not tested yet: the speed with a
  real Linux graphics driver, Proton and DXVK, controllers and the Steam Deck. Problems are expected; the issue
  form asks for the distribution, the Wine or Proton version and the graphics driver.

### Fixed

- Under Wine the vertex shaders did not compile (they used `isnan`, which Wine's shader compiler does not know), so
  every vertex was transformed on the processor; they go to the graphics card again.
- Closing the window, or Exit in the in-game menu, now ends the program at once, every part of it. Under Wine the
  process stayed in memory after the window had closed, with one of the game's threads busy at full speed.
- When Direct3D 11 cannot start, the launcher crashed while falling back to asking for the disc with a dialog.

## 0.2.3 (2026-11-01)

### Improved

- The zip and the installer now contain `README.txt`, a short guide to what you need, the first start, where your
  files are and what to do if something goes wrong, and `CHANGELOG.md`. The installer offers to open `README.txt`
  at the end.
- The README's requirements say what is and is not needed (nothing to install besides Windows 10 or 11; Windows N
  editions need the Media Feature Pack; an audio device is optional) and a troubleshooting section covers the
  common cases.
- When `mc3runtime.dll` does not load, the message gives Windows' error and the likely causes (a missing Windows
  component, a quarantined file) instead of only saying where the file must be.
- The log starts with what a report needs: mc3port's version, Windows' version and build, processor, memory, and
  every graphics adapter with its video memory and driver version. A crash names the module it happened in (or the
  game's code), not only the address. A graphics resource the driver refuses to create is logged with what was
  asked for, where before it failed silently.
- The zip now contains a folder `mc3port` instead of loose files; the folder has the same name in every version,
  so a new zip extracted in the same place updates the old one.
- Cleaner sound when many sounds play at once, as in the city: the level comes down smoothly just before a peak
  instead of each peak being rounded off, which made the sound harsh and crackly; the overall level is a little
  lower, so this happens less often.

### Fixed

- The likely cause of a crash at random moments in the city during the career, inside the game's sound engine. On
  the Xbox's single processor no other part of the game runs while the system is in the middle of certain work
  (interrupts, sound updates); the port now keeps that rule even when Windows would switch to another part of the
  game meanwhile.
- MC3 parks its notification box just off the left edge, where a television hid it: its last pixels stayed on
  screen. Like a TV, the outermost edge of the picture (1/80 of each side) is no longer shown.
- The Motion blur setting now also covers the blur MC3 adds at high speed, the streaks across the picture and
  from the street lights: at 0 they are gone, where before they stayed whatever the setting.
- Linux with Wine or Proton is still unsupported, but two known reasons for failing there are gone. Without an
  audio output device the launcher closed at once (Wine's XAudio2 crashes where Windows' reports an error): the
  port now checks for an output device first and runs silent without one. Wine reports four privileged
  instructions (wbinvd, invd, rdmsr, wrmsr) as a memory fault, which the port now recognises and emulates as on
  Windows. The log says when the program runs under Wine, with the Wine and host versions.

## 0.2.2 (2026-10-27)

### New

- A new program icon and logo, in the spirit of the game's: chrome "MC" and a big red "3".
- Controller buttons as icons (A, B, X, Y in their colours, bumpers, triggers, sticks, d-pad, Start and Back) in the
  Controls page, in the bottom bar when a controller is connected, and in the hint at the start of the game.
- The Controls page names what each button does in the game (handbrake, weight transfer, nitrous, look back,
  camera, headlights, navigation system, map, hydraulics, music) instead of "A button", "B button".
- A key for the left stick click, the special ability (Zone, Agro or Roar): F by default. Before, the keyboard
  had no way to use it.

### Improved

- When a newer version is out, the launcher says so at the top of its first page (and of the first-run guide),
  with a pulsing marker, and marks the About entry of the menu; before, the notice sat low on the page.
- `mc3.exe` and `mc3runtime.dll` now carry their version number: Windows shows it in the file's properties, which
  helps when reporting a problem.
- Launcher polish: the Settings tabs' content fades in like a page; while choosing a key, Esc, a click elsewhere or
  B cancels, and the box says so (before, any key was taken, Esc included, and cancelling meant waiting);
  the side menu follows the mouse after a brief rest on an item instead of flipping pages as it passes; a hand
  cursor over everything clickable; with a controller the bottom bar says how to get into a page.
- The build is checked on every change, not only at release time.
- Smoother motion at 60 fps: a finished frame reaches the screen on time even while the next one is still being
  drawn. Before, about one frame in four came a refresh late and the following one early: the same frame rate, but
  uneven movement.
- The log notes every 10 seconds the game's frame rate, how evenly its frames come and how busy the game and the
  emulated graphics chip are, which helps when reporting a performance problem.

### Fixed

- Controllers in the launcher and the in-game menu: a controller on any port works (before, only the first), and
  one plugged in while playing is picked up. The left stick moves through menus and pages like the d-pad, one
  step at a time (held, it steps again after 0.4 s and then every 0.15 s); up and down in the menu repeat at the
  same pace from the keyboard and the d-pad.
- Back+Start opens the in-game menu when tapped; before, the two had to be held together.
- The launcher's pages no longer draw a white rectangle around the selected control: every control shows its own
  highlight, as the buttons already did.
- After an update, the desktop and Start menu shortcuts kept the old icon: the installer now asks Windows to
  refresh its icons.

## 0.2.0 (2026-10-27)

### New

- **In-game menu**: F1, or Back+Start on a controller. Settings, controls, resume and exit, in the launcher's style.
  The game pauses while it is open (timers, clocks and sound included) and resumes once the key is let go.
- In the in-game menu, settings the game reads only when it starts (language, frame rate, aspect ratio, quality,
  resolution, filtering) say so; the others apply at once.
- **Motion blur strength** (Settings, Effects): 0 off, 1 as strong as on the Xbox, 0.5 by default.
- **Frame counter** (Settings, Display), off by default.
- **Pause in the background**: switching to another window opens the in-game menu (on by default).
- A hint at the start of the game shows how to open the menu.

### Improved

- Launcher and menu controls work like a console's: up and down through the menu, right into the page, left or B
  back out, LB/RB (Q/E) switch tabs, Start plays. The bottom bar lists every key, for the keyboard or the connected
  controller.
- Long setting descriptions wrap instead of running under the controls.

### Fixed

- At 60 fps the game ran up to twice as fast while driving. Game time now follows real time at 30 and 60 fps.
- The game's motion blur read an empty picture; it now reads the frame, as on the Xbox.
- Keyboard and controller navigation skipped the launcher's buttons.

## 0.1.1 (2026-10-27)

First public release.

- Native Windows port of the European Xbox release, run from your own disc image.
- 16:9, 60 fps, internal resolution up to 4x, anisotropic filtering, optional modern effects.
- Launcher with disc check, first-run guide, settings, rebindable keys and updates; installer and portable zip.
- Fixed: the launcher closed when clicking "Choose ISO" (in the withdrawn 0.1.0).
