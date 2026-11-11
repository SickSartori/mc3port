// Custom soundtracks: the ST.DB layout the XAPI reads (checked against its code in MC3's XBE, 0x324F05 to 0x325325), and
// a conversion to WMA when Windows has the encoder.
//   test_soundtracks                       the checks
//   test_soundtracks <source> <MUSIC>      makes MUSIC from a folder of audio files, to try in the game

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include "pc/soundtracks.h"
#include "test.h"

using namespace xport::pc;

namespace {

uint32_t at(const std::vector<uint8_t>& data, size_t offset) {
    uint32_t value = 0;
    std::memcpy(&value, &data[offset], 4);
    return value;
}

std::wstring name_at(const std::vector<uint8_t>& data, size_t offset) {
    return std::wstring(reinterpret_cast<const wchar_t*>(&data[offset]));
}

// A 16-bit mono WAV of `seconds` of a 440 Hz tone at `rate`.
void write_wav(const std::filesystem::path& file, uint32_t rate, double seconds) {
    const uint32_t samples = static_cast<uint32_t>(rate * seconds), bytes = samples * 2;
    std::ofstream out(file, std::ios::binary);
    const auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4), u32(36 + bytes), out.write("WAVEfmt ", 8), u32(16), u16(1), u16(1), u32(rate), u32(rate * 2),
        u16(2), u16(16), out.write("data", 4), u32(bytes);
    for (uint32_t i = 0; i < samples; ++i) u16(static_cast<uint16_t>(static_cast<int16_t>(8000 * std::sin(i * 6.2832 * 440 / rate))));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3) {
        std::vector<MusicPlaylist> playlists;
        size_t failed = 0;
        for (const auto& [name, files] : folder_playlists(argv[1])) {
            MusicPlaylist playlist{name, {}};
            const MusicImport import = import_songs(files, argv[2], playlist, [](size_t done, size_t total, const std::wstring& file) {
                std::wprintf(L"%zu/%zu %ls\n", done + 1, total, file.c_str());
                return true;
            });
            failed += import.failed;
            if (!playlist.songs.empty()) playlists.push_back(std::move(playlist));
        }
        const std::string error = save_music(argv[2], playlists);
        std::printf("%zu playlists, %zu failed%s%s\n", playlists.size(), failed, error.empty() ? "" : ": ", error.c_str());
        return playlists.empty() || !error.empty();
    }

    // Two soundtracks: 7 songs (two groups) and 1 with a long name.
    std::vector<Soundtrack> soundtracks(2);
    soundtracks[0].name = L"Road trip";
    for (int i = 0; i < 7; ++i) soundtracks[0].songs.push_back({L"Song " + std::to_wstring(i), 1000u + i});
    soundtracks[1].name = L"A name much longer than thirty-one characters";
    soundtracks[1].songs.push_back({L"Only", 5000});
    const std::vector<uint8_t> db = soundtrack_database(soundtracks);
    CHECK(db.size() == (101 + 3) * 512u);
    CHECK(at(db, 4) == 2);                                  // soundtracks
    CHECK(at(db, 0xC) == 1 && at(db, 0x10) == 2);           // their ids, from 1
    CHECK(at(db, 512) == 0x21371 && at(db, 512 + 4) == 1);  // soundtrack 1 at block 1
    CHECK(at(db, 512 + 8) == 7);                            // songs
    CHECK(at(db, 512 + 0xC) == 0 && at(db, 512 + 0x10) == 1);  // its groups: global ids 0 and 1
    CHECK(at(db, 512 + 0x15C) == 7000 + 21);                // total ms
    CHECK(name_at(db, 512 + 0x160) == L"Road trip");
    CHECK(at(db, 1024 + 4) == 2 && at(db, 1024 + 0xC) == 2);  // soundtrack 2: its group is the third
    CHECK(name_at(db, 1024 + 0x160).size() == 31);
    const size_t g0 = 101 * 512, g1 = 102 * 512, g2 = 103 * 512;
    CHECK(at(db, g0) == 0x31073 && at(db, g0 + 4) == 1 && at(db, g0 + 8) == 0);
    CHECK(at(db, g0 + 0x10) == 0x10000 && at(db, g0 + 0x10 + 20) == 0x10005);  // song ids, never 0
    CHECK(at(db, g0 + 0x28) == 1000 && name_at(db, g0 + 0x40) == L"Song 0");
    CHECK(at(db, g1 + 8) == 1 && at(db, g1 + 0x10) == 0x10006 && at(db, g1 + 0x14) == 0);  // songs 6.., then empty
    CHECK(at(db, g2 + 4) == 2 && at(db, g2 + 0x10) == 0x20000);
    {
        wchar_t temp_folder[MAX_PATH];
        GetTempPathW(MAX_PATH, temp_folder);
        const std::filesystem::path music = std::filesystem::path(temp_folder) / L"mc3port-test-music";
        std::filesystem::create_directories(music);
        std::ofstream(music / L"ST.DB", std::ios::binary).write(reinterpret_cast<const char*>(db.data()), db.size());
        const std::vector<MusicPlaylist> loaded = load_music(music);
        CHECK(loaded.size() == 2 && loaded[0].songs.size() == 7 && loaded[1].songs.size() == 1);
        CHECK(loaded[0].songs[6].file == music / L"0001" / L"00010006.WMA" && loaded[1].songs[0].milliseconds == 5000);
        // A save stopped between its renames left MUSIC aside: the next load puts it back.
        std::filesystem::rename(music, std::filesystem::path(music.wstring() + L".old"));
        CHECK(load_music(music).size() == 2);
        std::filesystem::remove_all(music);
        CHECK(load_music(music).empty());  // none
    }
    {  // read back as written
        const std::vector<Soundtrack> back = parse_soundtrack_database(db);
        CHECK(back.size() == 2 && back[0].name == L"Road trip" && back[0].songs.size() == 7);
        CHECK(back[0].songs[6].name == L"Song 6" && back[0].songs[6].milliseconds == 1006);
        CHECK(back[1].name.size() == 31 && back[1].songs.size() == 1 && back[1].songs[0].milliseconds == 5000);
        CHECK(parse_soundtrack_database(std::vector<uint8_t>(100)).empty());
    }
    CHECK(soundtrack_song_path(0, 0) == std::filesystem::path(L"0001") / L"00010000.WMA");
    CHECK(soundtrack_song_path(1, 6) == std::filesystem::path(L"0002") / L"00020006.WMA");

    // A short 48 kHz mono tone to WMA, when this Windows can encode it (not Windows N without its pack, not Wine).
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::filesystem::path folder = std::filesystem::path(temp) / L"mc3port-test-soundtracks";
    std::filesystem::create_directories(folder);
    write_wav(folder / L"tone.wav", 48000, 1.5);
    uint32_t ms = 0;
    std::string error;
    if (convert_to_wma(folder / L"tone.wav", folder / L"tone.WMA", ms, error)) {
        CHECK(ms >= 1400 && ms <= 1600);
        CHECK(std::filesystem::file_size(folder / L"tone.WMA") > 1000);
        // The manager: a folder imported, a playlist added, a song and a playlist deleted, each saved.
        const std::filesystem::path music = folder / L"MUSIC", source = folder / L"source";
        std::filesystem::create_directories(source / L"Mix");
        std::filesystem::copy_file(folder / L"tone.wav", source / L"Mix" / L"a.wav");
        write_wav(source / L"Mix" / L"b.wav", 44100, 2.5);
        std::vector<MusicPlaylist> playlists;
        for (const auto& [name, files] : folder_playlists(source)) {
            MusicPlaylist playlist{name, {}};
            CHECK(import_songs(files, music, playlist, nullptr).songs == 2);
            playlists.push_back(std::move(playlist));
        }
        CHECK(playlists.size() == 1 && playlists[0].name == L"Mix" && playlists[0].songs[1].name == L"b");
        CHECK(save_music(music, playlists).empty());
        CHECK(std::filesystem::exists(music / L"0001" / L"00010001.WMA"));
        CHECK(!std::filesystem::exists(folder / L"MUSIC.import"));  // the conversions moved in
        playlists = load_music(music);
        playlists.push_back({L"My songs", {}});
        const MusicImport added = import_songs({folder / L"tone.wav", folder / L"not audio.txt"}, music, playlists[1], nullptr);
        CHECK(added.songs == 1 && added.failed == 1);
        CHECK(save_music(music, playlists).empty());
        playlists = load_music(music);
        CHECK(playlists.size() == 2 && playlists[1].name == L"My songs" && playlists[1].songs.size() == 1);
        // The first song goes: the second takes its file name, with its own sound (2.5 s).
        playlists[0].songs.erase(playlists[0].songs.begin());
        CHECK(save_music(music, playlists).empty());
        playlists = load_music(music);
        CHECK(playlists[0].songs.size() == 1 && playlists[0].songs[0].name == L"b");
        CHECK(playlists[0].songs[0].milliseconds >= 2400 && !std::filesystem::exists(music / L"0001" / L"00010001.WMA"));
        CHECK(std::filesystem::exists(music / L"0001" / L"00010000.WMA"));
        // The first playlist goes: the second becomes 0001.
        playlists.erase(playlists.begin());
        CHECK(save_music(music, playlists).empty());
        playlists = load_music(music);
        CHECK(playlists.size() == 1 && playlists[0].name == L"My songs" && !std::filesystem::exists(music / L"0002"));
        CHECK(std::filesystem::exists(music / L"0001" / L"00010000.WMA"));
        CHECK(save_music(music, {}).empty() && !std::filesystem::exists(music));  // none left: no MUSIC
    } else {
        std::printf("conversion not checked: %s\n", error.c_str());
    }
    std::filesystem::remove_all(folder);
    return test_result();
}
