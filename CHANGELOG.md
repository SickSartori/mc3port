# Changelog

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
