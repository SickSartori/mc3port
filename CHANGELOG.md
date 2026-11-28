# Changelog

## 0.4.2

### New

- **Mods inside the game's archives**: a file in the folder `mods` with the path it has inside `assets_x.dat` to
  `assets_x.dat_5` (the game's tuning, texts, cars, cities) is read instead of the archive's, as the disc's files
  already were: `mods\tune\vehicle\vehicle.lst`, `mods\resources\vehicle\vp_350z_04\vp_350z_04.xbck`. A file the
  archives do not have is added, into the archive with the deepest of its folders: a new car's files beside the
  others, with the whole `vehicle.lst` and its block added at the end; `mc3port.log` names each one.
- **Mods folder** in the launcher's About page: opens the folder, made empty the first time.

### Fixed

- **Smoother where the city is busy**: the drawing thread no longer checks the game's memory for changed textures
  itself (another thread does, as often): about 80 ms of its work a second less while driving.
- **Settings that apply at once**: the texture filter and the HUD (mc3port's or the game's) change while you play;
  only the HD HUD still needs a restart.
- **No more short hitches at the same places of every race** (issue 29): the music's reads from the disc image no
  longer wait for the city's; each part of the game reads the image on its own. A read that still holds the game up
  says in the log how much of its time it waited for another.
- **The game no longer freezes switching cars in the garage** (or opening the pause menu): the game's graphics code
  could go to sleep waiting for a signal that, with the drawing on its own thread, had already gone by.
- **No more flicker at 21:9**: at high frame rates some frames of a race were taken for a loading screen and shown
  at 16:9 for an instant (the game draws the next frame while the last one waits to be shown).

## 0.4.1 (2026-11-26)

### New

- **Português (Brasil)** as the game's language (settings, Game; also RecompLauncher): João13 e cia's translation
  of the Xbox game, with their credits in the About page and in `translations/README.md` (CC BY-NC 4.0), added at
  the suggestion of NAKADANI's video. The first time it is chosen, mc3port makes the translated files from your disc
  (about 1.1 GB in `translations\pt-br` in the data folder, some seconds); the European and the USA discs both work.
  The redrawn HUD has its start word too.
- **Controller rumble** in the settings (Game), also in the in-game menu: how strongly the controllers rumble, from 0
  (off) to 100% of what the game asks, the default. It changes at the game's next rumble.
- **Gamma and brightness** in the settings (Effects), also in the in-game menu, for a screen that shows the game too
  dark or too light: they change the 3D scene only, at once, with the modern effects on or off; the HUD, the menus and
  the films stay as they are. No style changes them.

- **A third HUD, HD: the game's own redrawn**: in the settings (Game) the HUD is now the game's, HD or
  mc3port's. HD is the game's HUD with its pictures drawn again sharp, in the folder `hud_redrawn` beside mc3port:
  the dials of the Jetta and the McLaren, the radar, the arrows, the digits, the countdown, the side arcs and the
  start's word in each language. The other dials and the fonts stay the game's for now.
  Its side arcs (the special ability's over the radar, the slipstream's and the nitrous's beside the dials) are
  smaller, so they end above the radar and the dials instead of running under them.

### Improved

- **Report the crash stands out**: after a crash, the button over the bar has an amber border and a ring that
  widens and fades every second and a half, so the report is not missed.
- **A setup step on RecompLauncher**: after your disc image is checked, the launcher makes sure Direct3D 11 starts
  on this PC (a few seconds, no window), so a graphics card or a Wine without it is named at setup instead of
  with a black window at the first start. The game's page also lists the disc image the newer way (named inputs),
  which RecompLauncher 1.0 will need.
- **mc3port's HUD, the radar**: its ring is drawn again in the dials' style, a dark band with a line in the HUD's
  colour, and the N in a disc like the gear's, turning with the map as the game's tab does. The map and its icons
  stay the game's.
- **More settings on RecompLauncher's page**: 120 frames per second, 21:9, the field of view, the texture filter,
  sharp textures, the modern effects and the frame counter, which the launcher and the in-game menu already had.
- **Replacement pictures with the game's colours**: a PNG with a palette in `textures_replace`, for a picture of the
  game that has one (the speedometer, the rev counter), is coloured from the game's palette. A redrawn dial keeps the
  HUD colour chosen in the garage.

### Fixed

- **The window mode in the settings after F11 or Alt+Enter**: the row showed the mode in `mc3port.ini`, not the
  window's own, so after F11 the in-game menu could say "Window" in full screen. It now shows the window as it is.
  The settings also say that full screen has no border, which it always had: Alt+Tab is instant.
- **mc3port's HUD, the special ability**: the arc over the radar that charges Zone, Agro or Roar is now drawn by the
  new HUD too, sharp at any resolution and in the HUD's colour; it stayed the game's thin one. The arc beside the
  speedometer that was described as the special ability is the slipstream turbo (it fills behind an opponent and is
  used with the nitrous button): it is named so now. The marks of the charges you have stored stay the game's.
- **mc3port's HUD, the damage meter**: the red pieces that light one by one around the speedometer as the car takes
  heavy hits were left out, taken for a rev limiter light. The new HUD draws them again, at the lower left of its
  speedometer.

## 0.4.0 (2026-11-25)

### New

- **21:9** (issue 18): a third aspect ratio next to 4:3 and 16:9 in the launcher's and the in-game menu's settings. On a
  screen wider than 16:9 (21:9, 32:9) the game takes your screen's shape and sees more to the sides, as it does in 16:9
  compared with 4:3; the radar and the dials stay round. Films and loading screens are made for 16:9 and keep bars
  at the sides. The game's own texts and prompts come out a little wider than they should.
- **Field of view**, in the same settings: how wide the camera sees, from 75% to 130% of the game's own. It changes at
  once, also from the in-game menu.

- **120 frames per second**: a third choice next to 30 and 60 in the launcher's and the in-game menu's settings. The
  game runs at the same speed, with twice the frames of 60. It needs a fast processor (one of its cores does the
  work twice as often) and a screen of 120 Hz or more to show them; where the PC cannot keep up, the game stays smooth
  at the frames it reaches.
- **Sharp textures** (off by default, in the same settings): the textures of roads, buildings and cars enlarged two
  times with their edges kept, as Sharp pictures does for the HUD. It shows from close: signs and markings lose their
  steps. Nothing is added that the game does not have, and from afar the picture is the game's own. The textures
  sharpen a few seconds after a place loads, and take more graphics memory.
- **PS2 loading-screen music** (#24, by Nekoniria), optional: the PS2 version's loading music on the loading screens,
  from WAV files you supply (none come with mc3port), best with a longer loading-screen duration. The launcher's Music
  page has a PS2 Loading Music tab: import a folder with `1.wav` to `18.wav`, or the PS2 disc's own `1.raw` to `18.raw`
  (#25, converted to WAV as they are imported), the volume, the fade-out, the order (in
  order, random, or weighted by each track's own weight) and a preview. The files go in `loading_music` in the game's
  data folder; `docs/loading_music/README.md` in the package says how. RecompLauncher's page turns it on and off and
  sets its volume.

### Improved

- **More natural light and colour from the modern effects**: the glow, contrast, saturation and vignette now apply to
  the 3D scene before the HUD is drawn, so the HUD keeps its own colours. Contrast follows a curve that keeps
  detail in the darkest and brightest parts (before, night scenes lost their dark detail and lights burnt
  out), and the glow around lights is softer and wider.
- **The launcher's buttons**: Report a problem and Support the project sit above the bottom bar, on every page and in
  the in-game menu, and a speaker at the left of the bar turns the launcher's music off and on (`launcher_mute` in
  `mc3port.ini`; the game's volume stays in the settings). The hints' keys are drawn as keyboard keys. Support goes to
  Ko-fi only.
- **First start**: after the disc and the style the launcher opens its menu, with every page, instead of starting the
  game. The style is chosen from three cards that say what each one turns on.
- **Drawing on two threads**: one reads the game's drawing commands, the other makes the picture with Direct3D; before,
  one thread did both and was the limit of the frame rate. On the PC it was measured on, with the two ways taking
  turns in the same race: 50 frames a second instead of 37 in two-player split screen, 68 instead of 54 with one
  player at 120 fps. It needs four processor threads or more; `render_thread = 0` in `mc3port.ini` goes back to one
  thread, should the picture or the game misbehave. The two threads each have processor cores of their own: with
  Windows choosing, a test had some 10-second stretches in the city at 85 frames a second where they held 120.
- **A little faster again where the PC works hardest**: for each draw the textures and the vertex state are found
  without being looked up again, a few percent less work on the thread that limits the frame rate.
- **Replacement pictures for any texture**, not only the HUD's: `MC3_DUMP_TEXTURE=all` saves every texture the
  game draws with as a PNG in a folder `textures_dump` next to `mc3port.ini`. Redrawn or enlarged with a tool of
  your own and put in `textures_replace` under the same name, at any size, they are drawn instead of the game's:
  the way to textures with more detail than the game has (most of its own are 128 x 128 pixels).
- **The launcher's new layout**, in the in-game menu too: each page sits in a column from the top of the window to the
  bottom bar, with Play at its foot. A page longer than the window scrolls (the mouse wheel, or the selection kept in
  sight with the keys and the controller): the Picture tab's last rows were hidden under the bottom bar on a 720p
  window. In a 4:3 or smaller window everything is larger than before (it was sized for 16:9 and shrank). A slider's
  value now sits beside its grab instead of under it, the rows of a page come in one after another, a light runs round
  Play when the game is ready and along the menu's selected item. The internal resolution is a drop-down list, like
  the game language, instead of seven narrow buttons.
- **Settings in five shorter tabs**: Picture, with ten rows, is now Display (aspect ratio, field of view, window, frames
  per second, VSync) and Graphics (internal resolution, texture filter, sharp pictures and textures, vertices on the
  graphics card), next to Effects, Game and Audio.
- **The launcher's light trails** run on three planes, the nearer ones lower, thicker and brighter, and come more
  often and faster when the music gets louder.

- **Fewer pauses the first time you reach a place**: the shaders a scene needs the first time are made on two helper
  threads instead of in the middle of a frame; a frame waits for them at most 8 ms, and what is still missing is
  drawn a frame or two later. Entering the city with no shaders made yet ran at about 42 fps instead of 32 in tests.

- **Faster where the PC works hardest**: the work that turns each frame's Xbox drawing commands into Direct3D takes about
  a quarter less time than in 0.3.9, which counts most in two-player split screen, where the city is drawn twice, and at 120
  frames per second.
- **Processors with fast and slow cores** (Intel's P and E cores): the game and its drawing work stay on the fast ones,
  each on a core of its own. They could land on a slow core, or share one, and the frame rate swung with it.
- **Full speed in the background**: Windows no longer slows mc3port down when its window is not in front or the
  battery saver is on.
- **An incomplete disc image is named in the log** (issue 22): when a file or folder of the image lies past its end (a
  download cut short), or a read from the disc fails, `mc3port.log` says so. Before, it showed only as a crash later.
- **Linux and macOS**: at start the port checks that the picture it hands from one Direct3D device to the other
  really arrives, and shows it the plain way where it does not. Not yet tried on those systems.
- **Fewer antivirus false alarms** (VirusTotal flagged the 0.3.9 package with 4 engines). Started by RecompLauncher, `mc3.exe` no longer starts the game
  process suspended and then resumes it, which is how malware hides its code in another process: the game process
  starts as usual and joins `mc3.exe`'s job at once. `mc3.exe` and `mc3runtime.dll` also carry the checksum in
  their header, which was left empty.
- **Code signed files**: `mc3.exe`, `mc3runtime.dll`, the installer and its uninstaller are signed (Windows shows the
  publisher "Open Source Developer Gabriel Sartori"), which Windows and antivirus programs trust more than
  unsigned files. The certificate is new, so SmartScreen may still ask once until it has seen enough downloads.

### Changed

- **A new launcher music**, made the same way as the one before it and under the same licence (CC BY 4.0, see
  THIRD_PARTY.md).
- **Updating from the launcher**: Update opens the new installer's link and your browser downloads it; you start it,
  as any download. The launcher no longer downloads and starts the installer itself, which antivirus programs take
  for the way malware brings in more of itself.
- **The keyboard is read from the game's window**, no longer by asking Windows for the whole keyboard's state, which
  antivirus programs also distrust. Keys pressed in other windows no longer reach the game.
- **Fewer files to choose from in a release**, with names that say what they are: `-windows-setup.exe` (the
  installer), `-windows-portable.zip` and `-linux.tar.gz`, and a "Which file?" list at the end of the release notes, right above the files. The
  installer is no longer attached a second time in a zip, and "x86" is gone from the names.

### Fixed

- **Frame timing**: a vertical blank was at times counted just after the frame that came with it, so two frames of
  the same length were timed as one short and one long, for the game's clock and in the log. Now every frame at 60
  fps counts its two blanks, and the blanks come from a more precise timer.
- **Sharp pictures on the HUD's smallest pictures**: those enlarged four times got one level too many, Direct3D
  refused them (`cannot create an enlarged texture` in the log) and they stayed as the game's. Now they are sharp
  like the others.
- **White blotches over the launcher's background**: some of the disc's pictures have cut-out shapes, whose hidden
  parts were drawn in white. They are now dark, as the rest of the background.
- **No pale box around pedestrians and lamp posts at night**: the ambient occlusion lightened the shaded wall behind
  anything standing in front of it, in a rectangle as wide as its radius. It now shades the wall right up to the figure.

## 0.3.9 (2026-11-17)

### Improved

- **RecompLauncher** marks a disc image as verified when it is a Redump copy of a supported release: `recomplauncher.json`
  lists their size and CRC32. Other images (trimmed ones, for example) play as before.

### Changed

- **The compiled shaders no longer ship with mc3port**: they are made from the game's own and are not built from this
  repository's source, which code signing needs. Your PC compiles each one the first time it is needed (a short pause,
  for example on entering a city) and keeps it in `Documents\mc3port\shader_cache`, as before 0.3.7.
- **The launcher music is under CC BY 4.0**, so every part of mc3port has an open licence.

### Fixed

- **Uninstalling removes the whole folder**: an empty `setup_choices.ini` kept it there.

## 0.3.8 (2026-11-17)

### New

- **Support the project** from the About page of the launcher and of the in-game menu: a row with the links to GitHub
  Sponsors and Ko-fi. It is never needed to play and it unlocks nothing; there are no reminders.
- **PlayStation buttons**: with a DualSense, DualShock 4 or DualShock 3 connected (or RecompLauncher's PlayStation
  pad when none is), the launcher and the in-game menu show the cross, circle, square and triangle, L1, R1, L2, R2,
  Options and Create (Share, Select) instead of the Xbox's buttons. The game's own prompts stay the Xbox's.

### Improved

- Choosing a file that is not a disc image (one of mc3port's own files, for example) now says that the game is not
  included and that your own `.iso` or `.cci` is needed, instead of only "not an Xbox disc image".
- **When Direct3D 11 does not start**, mc3port says so and what to do (the graphics driver on Windows, the 32-bit
  Vulkan libraries under Wine) instead of showing a black window.
- **Linux**: the install script checks that Direct3D 11 starts in your Wine and lists the packages to install when it
  does not. The log says which Direct3D is in use (Windows', Wine's or DXVK's) and whether Steam started the game.

### Fixed

- **The headlights light the road again** (issue #20): a soft, warm pool of light in front of the car at night, a
  little brighter than on the Xbox (up to 15% added instead of 12.5%). The game counts its headlights' light volumes
  in a small surface against a copy of the scene's depth; the port tested them against an empty depth buffer, counted
  each face as a whole step instead of a small one, and then faded the result out as if it were the light streaks.
- **RecompLauncher**: `recomplauncher.json` follows the schema again (the launcher skipped it), keeps the settings,
  saves and logs where mc3port keeps them (`Documents\mc3port` unless portable) and adds Window, V-Sync, Language and
  5K. It also comes with the installer.
- Started by RecompLauncher, mc3port uses only the disc the launcher gives: no other disc in its place and no file
  dialog, but exit code 2, so the launcher asks for it again. Quitting the launcher reopened by "Back to the launcher"
  no longer ends with code 2.
- Started by RecompLauncher, the game's restarts (the in-game menu's restart, "Back to the launcher", moving the old
  data folder, a lost graphics card) no longer look to the launcher like the game had ended: `mc3.exe` stays as the
  process it started, starts each new one itself and ends with the game's exit code. In Big Picture mode the lost
  graphics card's message is not shown (a controller could not close it), only logged.
- The display no longer turns off while playing with a controller: Windows saw no input and let it sleep.

## 0.3.7 (2026-11-11)

### New

- **Your own music on the game's radio** (#17). The launcher has a **Music** page: make playlists from your MP3,
  FLAC, WAV, WMA or M4A files, add, rename and delete songs and playlists, or import a whole folder (each subfolder
  becomes a playlist). mc3port converts the songs to the Xbox's format, as the Xbox dashboard did. In the game, choose
  a playlist in Pause > Options > Audio Options, with the Music selector at the top.
- **Back to the launcher** in the in-game menu (F1): the game closes and the launcher opens again, where the disc,
  your music and the settings that need a restart can be changed.
- **Updates from the launcher.** When a new version is out, the launcher says so at start, with what is new. An
  installed copy downloads the installer (with its progress), checks it, and starts it when you press Install;
  nothing installs by itself. A portable copy opens the release page.
- **The launcher in Portuguese and Spanish**, chosen from Windows' language like English and Italian.

### Fixed

- **Micro stutter** when frames come late for a moment, for example while new textures enter the city. The game's
  step then jumped from 1/60 to 1/30 s for a frame and back, and the picture skipped ahead. The step now changes
  only when the frame rate clearly changes.
- **Stutter the first time each effect shows.** The graphics card's shaders were compiled while playing, up to a
  second each (87 of them, 8 seconds in all, from the first start to the garage). The shaders now come with mc3port
  and are made ready at start, on a thread of their own: a first run compiles none while playing.
- **Song titles in 16:9**: the first letters no longer drift towards the radar, and a long title no longer runs under
  the dials. The scrolling text of the first screen (the legal notice) keeps its normal spacing.

### Improved

- **The installer also comes in a zip** (`mc3port-<version>-windows-x86-setup.zip`). Chrome, Edge and Opera often
  block a new installer that is not code signed; the zip gets through. Extract it and run the setup. The plain
  `-setup.exe` stays in the release for the launcher's updates.
- The installer says what mc3port sends over the internet (only the update check, to GitHub) and has a box to turn
  the update check off.
- mc3port's shared runtime is now open source, as [xport](https://github.com/SickSartori/xport) (MIT licence), so
  every line of the Windows files can be read and built from public code. This is a first step towards code signed
  releases.

## 0.3.6 (2026-11-10)

### New

- **VSync** (Settings > Picture, off by default). Since 0.3.5 frames are shown without waiting for the screen, for
  the frame rate, and the picture can tear in turns and fast camera moves. With VSync on it never tears, but a PC
  that cannot keep up may drop to 30 fps. It takes effect at the next start.

### Improved

- Textures follow the sharpness the game asks for (the texture filter's level-of-detail bias), as on the Xbox.
- mc3port now shares its game-independent runtime and its Direct3D 11 renderer with sm2port (the Spider-Man 2 port),
  so fixes to either reach both. Nothing changes in how the game plays.
- The launcher names the chosen disc's release (Europe or USA) instead of always "PAL", and the package's readme no
  longer says the USA release is unsupported.
- A refused disc image now names its region (North America, Japan, Europe and the rest of the world) in the message.
- The crash log also shows the caller's stack frame, to make crash reports easier to trace.

### Fixed

- **Textures broken and flickering with RTSS** (issue #6), and with any overlay that draws inside Present (Steam,
  Discord, MSI Afterburner). Since 0.3.4 frames are presented on a thread of their own, and the overlay drew with the
  game's own Direct3D device while the game was drawing: it swapped render targets and textures under the game's
  draws, and the road and buildings came and went. The window now has a Direct3D device of its own for presenting,
  so an overlay never touches the game's. `MC3_TEST_PRESENT_HOOK=1` acts as such an overlay, to check it without one.

- **Grainy neon glow** (issue #14). The ambient occlusion darkened the road under the car with a fine per-pixel
  pattern, which showed through the bright glow. It now uses the same directions for every pixel and comes out smooth.

## 0.3.5 (2026-11-09)

### New

- **PlayStation, Switch and other controllers** work directly, through SDL3, with the buttons in the Xbox positions:
  no more Steam Input or DS4Windows. Xbox controllers work as before.

### Improved

- **Frames are presented without waiting for the display**, where DXGI allows it (Windows 10 1607 and later). A frame
  that took a little longer than the display's refresh used to wait for the next one: on a laptop whose dedicated GPU
  draws into a display wired to the integrated one, the city sat at exactly 30 fps with the GPU half idle, and the
  frame rate dips on other PCs came from the same wait. The city now runs at 60 fps there.
- **Less work per draw**: Direct3D's state cache was emptied before almost every draw by a lookup that should not
  have touched it, so each draw re-set every render state. Lighter on the GPU thread, which was the limit on
  laptops.
- **The USA release** of the Remix is accepted, next to the European one: its code differs by one branch and every
  patch of the port applies to it. The log names the release.
- **Automatic internal resolution** stays at 1080 lines on integrated graphics (under 1 GB of its own memory), instead
  of following a 1440 or 2160 line monitor it cannot fill.
- **Laptops with two GPUs** render on the fast one. Windows hands programs the integrated GPU unless told otherwise,
  so the game ran on it, at the monitor's resolution, with the dedicated card idle. The log says which GPU draws
  (`renderer: on ...`); a choice made for mc3.exe in Windows' graphics settings still wins.

### Fixed

- **A wait for several objects with a time limit** could spin on one core for good once the limit passed with only
  some of the objects ready: the game then stopped for good. The limit now ends the wait.
- **The menu tap of Start or Back** on a controller was lost about half the time: the port's Back+Start menu
  combination replayed the tap into a read the game never looked at.
- **mc3port.ini** is written beside itself and renamed over, so a crash while a slider moves cannot leave it empty
  (disc path, keys and settings lost).
- **A damaged disc image or default.xbe** with absurd counts or sizes closes the launcher no more: they are checked
  before anything is read or allocated. A save name with a drive letter or a byte outside ASCII is refused instead
  of reaching the PC's file system or ending the game.
- **Frames stuck on a missed vertical blank**: Direct3D retires a flip only when its count of blanks is exactly the
  flip's target, and on a PC the count can skip one (the game's own poll and the port's interrupt both count the same
  blank). The flip is now retired at or past its target.
- **"Checking saved games" stuck for good at 60 fps**: the game's clocks counted from Windows' start instead of the
  game's. After about six days of uptime (three with 0.3.4's step; Windows' fast startup keeps the uptime across a
  shutdown) the menus' clock, seconds kept as a float, had grown so large that adding 1/60 s no longer changed it: it
  stood still and the screen waited for ever for its half second. At 30 fps the larger step still got through, which
  is why it only hit 60 fps, and only some PCs. The clocks now start with the game, as an Xbox's with the console.
  Reproduced on a laptop up for a week and fixed there, with the European and the USA disc (issues #4, #6, #11, #12).
- **The cars' neon underglow shows**, in the garage, the shop and the city. Its glow is drawn as quads with a colour
  but no normal while the lighting is still on from the car, and lit with no normal they came out as the scene's
  ambient, near black. As in Direct3D, vertices without a normal now keep their own colour (issue #14).
- **Black textures after a long session on integrated graphics**: the port kept every texture it had ever made, and
  for most formats a new copy each time the draws before had used another palette, so after an hour the graphics
  memory ran out and new textures failed to appear. Textures no draw has asked for in a minute are now let go.
- **The fixed time step at 60 fps** is a whole number of sixtieths, a steady 1/60 s: on a PC whose frames come a
  little unevenly (now 1 blank, now 3) it followed the average and changed every frame, between 0.0156 and 0.0229 s.
- **Crash leaving the garage, or entering the career, as the music changes**: the game's WMA music decoder blends
  its blocks in a loop of 16 samples at a time that runs at least once. Asked for fewer than 16 samples (the rest
  has code of its own), it counted down from zero and ran on through memory, reading and writing past its buffers
  until a page was missing. Reproduced with a crash dump (the loop's counter at -2231); the loop is now skipped when
  it has nothing to do, with the European and the USA disc (issues #13, #15). The page after a game buffer is also
  always there now, as on the Xbox: reservations the game places itself fill their 64 KB granule, and a read or write
  just past a buffer gets a fresh page, the log saying so.

## 0.3.4 (2026-11-08)

### Improved

- The Cheats page has two tabs: **mc3port** with its own cheats (money, with a career loaded in the menu during the
  game) and **Game codes** with the game's codes to look up, one per line and easier to read.
- The log says what the game found in the save folder, one line per listing, for reports of a game stuck on "Checking
  saved games".

### Fixed

- **Frame limiters and overlays** (RivaTuner Statistics Server, MSI Afterburner) made the menus slow and the game judder
  at 60 fps: they wait inside the call that shows a frame, which also ran the emulated graphics card. That call now has a
  thread of its own.
- **Texts above the dials in 16:9** came apart ("GARA OPZION   ALE") with mc3port's HUD: part of a line was narrowed with
  the dials, the rest was not. Lines stay whole with both HUDs.

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
