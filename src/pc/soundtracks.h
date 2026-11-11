#pragma once

// Custom soundtracks, as the Xbox dashboard kept them: TDATA\FFFE0000\MUSIC on the hard disk, a database ST.DB and one
// folder of WMA songs per soundtrack. Games that support them (Midnight Club 3 among them: Pause > Options > Audio,
// the music selector) list and play them through the XAPI, which reads that database. Nothing here is MC3's.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace xport::pc {

struct SoundtrackSong {
    std::wstring name;
    uint32_t milliseconds = 0;
};

struct Soundtrack {
    std::wstring name;
    std::vector<SoundtrackSong> songs;
};

constexpr size_t kMaxSoundtracks = 100;
constexpr size_t kMaxSoundtrackSongs = 84 * 6;
constexpr size_t kSoundtrackNameLength = 31;  // UTF-16 characters, then a terminating zero

// A song's id: the soundtrack's (from 1) in the upper 16 bits, the song's index below. Never 0: a song id of 0 reads as
// no song to XACT, and the soundtrack stayed silent.
uint32_t soundtrack_song_id(size_t soundtrack, size_t song);

// Where a song's file goes under MUSIC: "<soundtrack id, 4 hex digits>\<song id, 8 hex digits>.WMA".
std::filesystem::path soundtrack_song_path(size_t soundtrack, size_t song);

// ST.DB for these soundtracks (at most kMaxSoundtracks, each at most kMaxSoundtrackSongs songs): 512-byte blocks, the
// header, one block per soundtrack from block 1, then groups of 6 songs from block 101.
std::vector<uint8_t> soundtrack_database(const std::vector<Soundtrack>& soundtracks);

// Converts an audio file Windows can read (MP3, FLAC, WAV, WMA, M4A/AAC) into WMA 44.1 kHz stereo at about 128 kb/s,
// through Media Foundation. `milliseconds` gets its length. False with `error` when it cannot (a file Windows does
// not read, or no WMA encoder: Windows N without the Media Feature Pack, Wine).
bool convert_to_wma(const std::filesystem::path& source, const std::filesystem::path& target, uint32_t& milliseconds,
                    std::string& error);

// The soundtracks of an ST.DB as written by soundtrack_database (names, songs and their lengths); none when unreadable.
std::vector<Soundtrack> parse_soundtrack_database(const std::vector<uint8_t>& data);

// --- the launcher's music manager: playlists (soundtracks) changed as a whole, then saved ---

struct MusicSong {
    std::wstring name;
    uint32_t milliseconds = 0;
    std::filesystem::path file;  // its WMA now: in MUSIC, or converted by import_songs and not saved yet
};

struct MusicPlaylist {
    std::wstring name;
    std::vector<MusicSong> songs;
};

// The playlists in `music` (the MUSIC folder), each song with its file there. It first tidies what a stopped save left:
// a MUSIC set aside goes back, a half-built one and unsaved conversions go.
std::vector<MusicPlaylist> load_music(const std::filesystem::path& music);

// Writes `playlists` as `music`: every song's file is linked (or copied) into a new folder under the name the database
// gives it, and that folder then takes the old one's place. Empty playlists are left out; none at all removes `music`.
// Empty when it worked; otherwise why, and the old folder is still whole.
std::string save_music(const std::filesystem::path& music, const std::vector<MusicPlaylist>& playlists);

struct MusicImport {
    size_t songs = 0, failed = 0;
    std::string error;  // the first file's problem (UTF-8)
};

// Converts `files` into WMA beside `music` (until save_music takes them) and adds them at the end of `playlist`, up to
// kMaxSoundtrackSongs. A song is named "Artist - Title" from the file's tags, or after the file. `progress` (done, total,
// file name) is called before each file; returning false stops there.
MusicImport import_songs(const std::vector<std::filesystem::path>& files, const std::filesystem::path& music,
                         MusicPlaylist& playlist, const std::function<bool(size_t, size_t, const std::wstring&)>& progress);

// The playlists a folder makes: its own audio files (named after the folder), then one per subfolder; files by name.
std::vector<std::pair<std::wstring, std::vector<std::filesystem::path>>> folder_playlists(const std::filesystem::path& source);

// MP3, FLAC, WAV, WMA, M4A or AAC, by the extension.
bool music_file(const std::filesystem::path& file);

}  // namespace xport::pc
