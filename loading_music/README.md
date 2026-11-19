# Optional PS2 Loading Music

Adds support for playing the original PS2 loading-screen music in Midnight Club 3: DUB Edition Remix.

## Installation

Place your own legally extracted PS2 loading-screen music files in the `loading_music` folder next to `mc3.exe`.

Name the files `1.wav` through `18.wav`. The feature automatically detects available valid tracks, so all 18 files are not required.

**Supported audio format:**
- Uncompressed 16-bit PCM WAV
- Mono or stereo
- Sample rates from 8,000 Hz to 192,000 Hz (24,000 Hz mono recommended for original PS2 tracks)

No copyrighted music files are included with mc3port. If no valid WAV files are found, the game continues normally without loading music.

## Configuration

The following options are available in `mc3port.ini`:

| Setting | Default | Description |
|---|---|---|
| `loading_music` | `1` | Enable (`1`) or disable (`0`) loading music. |
| `loading_music_volume` | `70` | Adjust loading music volume from 0 to 100. |
| `loading_music_fade_seconds` | `0.75` | Set the fade-out duration in seconds. |
| `loading_screen_seconds` | `0` | Set the minimum loading-screen duration in seconds. `0` uses the shortest supported duration of 2 seconds. |

Loading music also follows the game's overall volume setting.

## Playback Behavior

Loading music begins when a loading screen starts. Available tracks are played in sequence and loop when necessary.

Music continues for approximately one second beyond the configured minimum loading-screen duration, with a fade-out near the end.

**Note:** Playback timing is based on the minimum loading-screen duration rather than the exact visual transition. Depending on loading performance, music may briefly continue into gameplay or finish before the loading screen disappears.
