# Optional PS2 Loading Music

Adds support for playing the original PS2 loading-screen music in Midnight Club 3: DUB Edition Remix.

The feature includes launcher-based music importing, customizable playback modes, individual track preferences, and audio previews.

## Installation

### Option 1: Import Through the Launcher (Recommended)

1. Open the mc3port launcher.
2. Navigate to **Music → PS2 Loading Music → Settings**.
3. Click **Import Folder**.
4. Select a folder containing your legally extracted PS2 loading-screen music files named `1.raw`–`18.raw` or `1.wav`–`18.wav`.
5. The launcher validates file sizes and converts supported RAW files to WAV during import. Imported tracks are saved in the `loading_music` folder inside the mc3port game data directory (normally `Documents\mc3port\loading_music` on Windows).

The launcher displays how many valid tracks are installed. If both a WAV and RAW file exist for the same numbered track, the WAV is preferred. Existing installed tracks are not overwritten.

Imported tracks play from the next loading screen. Tracks copied into the folder by hand while the game runs do too: no restart is needed.

### Option 2: Manual Installation

Place your own legally extracted PS2 loading-screen music files in the `loading_music` folder inside the mc3port game data directory (normally `Documents\mc3port\loading_music` on Windows).

Name the files `1.wav` through `18.wav`.

The feature automatically detects available valid tracks, so all 18 files are not required.

### Supported Audio Format

**WAV playback and manual installation:**

- Uncompressed 16-bit PCM WAV
- Mono or stereo
- Sample rates from 8,000 Hz to 192,000 Hz
- 24,000 Hz mono recommended for original PS2 tracks

**Original PS2 RAW files (launcher import only):**

- Headerless, uncompressed signed 16-bit PCM
- 24,000 Hz, mono (one channel)
- Little-endian byte order
- File must contain at least one complete 16-bit sample (2 bytes), have an even byte count, and be no larger than 32 MiB minus 44 bytes for the WAV header
- The launcher adds a standard WAV header and copies the audio data in small chunks; it does not resample or change the samples

RAW files contain no format header. The importer can reject invalid file sizes, but cannot verify sample rate, channel count, byte order, or whether the contents are actually PCM. A file with a valid size but the wrong encoding may still produce noise. Only import RAW files in the format described above.

No copyrighted music files are included with mc3port.

If no valid WAV files are installed, the game continues normally without loading music.

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
| Import Folder | Import supported WAV files or convert original PS2 RAW files to WAV from a selected folder. |

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

Playback mode is configured through the launcher and stored in `mc3port.ini`. Individual track weights are stored separately in `loading_music/weights.txt` inside the mc3port game data directory.

Loading music also follows the game's overall volume setting.

## Playback Behavior

Loading music begins when a loading screen starts.

Available tracks are selected according to the configured playback mode and loop when necessary.

Music continues for approximately one second beyond the effective minimum loading-screen duration, with a fade-out near the end.

**Minimum duration of `0`:** The game still uses a shortest supported loading-screen duration of approximately 2 seconds, so loading music can play for approximately 3 seconds (including the extra one-second tail). Setting `0` does not disable loading music or make it stop immediately. For longer music playback, increase the minimum loading-screen duration in the launcher or set `loading_screen_seconds` to a higher value in `mc3port.ini`.

**Note:** Playback timing is based on the minimum loading-screen duration rather than the exact visual transition. Depending on loading performance, music may briefly continue into gameplay or finish before the loading screen disappears.
