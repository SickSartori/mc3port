# Optional PS2 Loading Music

Adds support for playing the original PS2 loading-screen music in Midnight Club 3: DUB Edition Remix.

The feature includes launcher-based music importing, customizable playback modes, individual track preferences, and audio previews.

## Installation

### Option 1: Import Through the Launcher (Recommended)

1. Open the mc3port launcher.
2. Navigate to **Music → PS2 Loading Music → Settings**.
3. Click **Import Folder** or **Import ZIP**.
4. Select a folder or ZIP archive containing your legally extracted PS2 loading-screen music files.
5. The launcher will detect and import supported tracks into the `loading_music` folder next to `mc3.exe`.

The launcher displays how many valid tracks are installed.

Restart the game after importing music if it is already running.

### Option 2: Manual Installation

Place your own legally extracted PS2 loading-screen music files in the `loading_music` folder next to `mc3.exe`.

Name the files `1.wav` through `18.wav`.

The feature automatically detects available valid tracks, so all 18 files are not required.

### Supported Audio Format

- Uncompressed 16-bit PCM WAV
- Mono or stereo
- Sample rates from 8,000 Hz to 192,000 Hz
- 24,000 Hz mono recommended for original PS2 tracks

No copyrighted music files are included with mc3port.

If no valid WAV files are found, the game continues normally without loading music.

## Launcher Configuration

All loading music settings can be configured through **Music → PS2 Loading Music** in the launcher.

### Settings

| Setting | Description |
|---|---|
| Enable Loading Music | Enable or disable loading-screen music. |
| Volume | Adjust loading music volume independently from the game's master volume. |
| Test Audio | Preview an installed track at the current volume settings. |
| Fade Duration | Configure how long the music takes to fade out. |
| Playback Mode | Choose Sequential, Random, or Weighted playback. |
| Import Folder | Import supported WAV files from a selected folder. |
| Import ZIP | Import supported WAV files from a ZIP archive. |

The Test Audio button allows you to listen to a track while adjusting the volume.

Audio previews loop until stopped and respect both the game's master volume and the loading music volume.

### Playback Modes

**Sequential**

Plays available tracks in numerical order, returning to the beginning after the last available track.

**Random**

Randomly selects from the available tracks.

**Weighted**

Randomly selects tracks based on individual track preferences, allowing certain songs to play more or less frequently.

Selecting Weighted playback enables the **Track Preferences** tab when valid music files are installed.

## Track Preferences

The Track Preferences tab allows you to customize the selection probability of each installed track.

Tracks are organized into three groups:

- Tracks 1–6
- Tracks 7–12
- Tracks 13–18

Each track has its own adjustable weight and Play/Stop preview controls.

### Track Weights

| Weight | Behavior |
|---|---|
| `0%` | Excludes the track from weighted selection. |
| `50%` | Gives the track half the default selection weight. |
| `100%` | Default selection weight. |
| `150%` | Gives the track 1.5 times the default selection weight. |
| `200%` | Gives the track twice the default selection weight. |
| `300%` | Gives the track three times the default selection weight. |

Weights are relative to other eligible tracks, not guaranteed playback percentages.

For example, if one track has a weight of 200% and another has 100%, the first track is twice as likely to be selected when both are eligible.

### Repeat Protection

Randomized playback includes repeat protection to prevent the same song from playing too frequently.

- A track may play a maximum of two consecutive times.
- After playing twice consecutively, that track cannot be selected again until three other tracks have played.
- If too few eligible tracks are available, playback can fall back to an available track rather than stopping entirely.

### Audio Previews

Each installed track has Play/Stop controls for previewing music directly in the launcher.

- Previews loop continuously until stopped.
- Playing another track stops the previous preview.
- Preview volume follows both the game's master volume and the loading music volume.
- Leaving the Music section or launching the game stops active previews.

When available, track titles may be displayed instead of numbered filenames. Otherwise, tracks are identified by their WAV filenames.

## Manual Configuration

The following settings are available in `mc3port.ini`:

| Setting | Default | Description |
|---|---|---|
| `loading_music` | `1` | Enable (`1`) or disable (`0`) loading music. |
| `loading_music_volume` | `70` | Adjust loading music volume from 0 to 100. |
| `loading_music_fade_seconds` | `0.75` | Set the fade-out duration in seconds. |
| `loading_screen_seconds` | `0` | Set the minimum loading-screen duration in seconds. `0` uses the shortest supported duration of 2 seconds. |

Additional playback mode and per-track weight settings are managed through the launcher and saved to the configuration file.

Loading music also follows the game's overall volume setting.

## Playback Behavior

Loading music begins when a loading screen starts.

Available tracks are selected according to the configured playback mode and loop when necessary.

Music continues for approximately one second beyond the configured minimum loading-screen duration, with a fade-out near the end.

**Note:** Playback timing is based on the minimum loading-screen duration rather than the exact visual transition. Depending on loading performance, music may briefly continue into gameplay or finish before the loading screen disappears.
