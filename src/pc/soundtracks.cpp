#include "pc/soundtracks.h"

#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propsys.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

namespace xport::pc {

using Microsoft::WRL::ComPtr;

namespace {

constexpr size_t kBlock = 512;
constexpr size_t kFirstGroupBlock = 101;  // after the header and the 100 soundtrack blocks
constexpr uint32_t kSoundtrackMagic = 0x21371, kGroupMagic = 0x31073;
constexpr size_t kGroupSongs = 6;

void put32(std::vector<uint8_t>& data, size_t offset, uint32_t value) { std::memcpy(&data[offset], &value, 4); }

void put_name(std::vector<uint8_t>& data, size_t offset, const std::wstring& name) {
    const size_t length = std::min(name.size(), kSoundtrackNameLength);
    std::memcpy(&data[offset], name.data(), length * sizeof(wchar_t));  // UTF-16 on Windows; the rest stays zero
}

std::string narrow(const std::wstring& text) {
    std::string out(WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                        nullptr),
                    '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), static_cast<int>(out.size()),
                        nullptr, nullptr);
    return out;
}

std::filesystem::path with_suffix(const std::filesystem::path& folder, const wchar_t* suffix) {
    std::filesystem::path out = folder;
    out += suffix;
    return out;
}

// The tags Windows reads from the file (System.Title, System.Music.Artist), defined here: propkey.h only declares them.
constexpr PROPERTYKEY kTitle{{0xF29F85E0, 0x4FF9, 0x1068, {0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9}}, 2};
constexpr PROPERTYKEY kArtist{{0x56A3372E, 0xCE9C, 0x11D2, {0x9F, 0x0E, 0x00, 0x60, 0x97, 0xC6, 0x86, 0xF6}}, 2};

std::wstring tag(IPropertyStore* store, const PROPERTYKEY& key) {
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring text;
    if (SUCCEEDED(store->GetValue(key, &value))) {
        if (value.vt == VT_LPWSTR && value.pwszVal) {
            text = value.pwszVal;
        } else if (value.vt == (VT_VECTOR | VT_LPWSTR) && value.calpwstr.cElems && value.calpwstr.pElems[0]) {
            text = value.calpwstr.pElems[0];  // the first artist
        }
    }
    PropVariantClear(&value);
    while (!text.empty() && iswspace(text.back())) text.pop_back();
    return text;
}

// "Artist - Title" from the file's tags, the title alone, or the file's name.
std::wstring song_name(const std::filesystem::path& file) {
    ComPtr<IPropertyStore> store;
    if (SUCCEEDED(SHGetPropertyStoreFromParsingName(file.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&store)))) {
        const std::wstring title = tag(store.Get(), kTitle), artist = tag(store.Get(), kArtist);
        if (!title.empty()) return artist.empty() ? title : artist + L" - " + title;
    }
    return file.stem().wstring();
}

std::vector<std::filesystem::path> audio_files(const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        if (entry.is_regular_file(error) && music_file(entry.path())) files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

// The WMA encoder's output type closest to 128 kb/s at 44.1 kHz stereo.
ComPtr<IMFMediaType> wma_output_type() {
    ComPtr<IMFCollection> types;
    if (FAILED(MFTranscodeGetAudioOutputAvailableTypes(MFAudioFormat_WMAudioV8, MFT_ENUM_FLAG_ALL, nullptr, &types))) {
        return nullptr;
    }
    DWORD count = 0;
    types->GetElementCount(&count);
    ComPtr<IMFMediaType> best;
    uint32_t best_distance = UINT32_MAX;
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IUnknown> element;
        ComPtr<IMFMediaType> type;
        if (FAILED(types->GetElement(i, &element)) || FAILED(element.As(&type))) continue;
        UINT32 rate = 0, channels = 0, bytes = 0;
        type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        type->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bytes);
        if (rate != 44100 || channels != 2) continue;
        const uint32_t distance = bytes > 16000 ? bytes - 16000 : 16000 - bytes;
        if (distance < best_distance) best_distance = distance, best = type;
    }
    return best;
}

struct MediaFoundation {
    bool started;
    MediaFoundation() : started(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {}
    ~MediaFoundation() {
        if (started) MFShutdown();
    }
};

}  // namespace

uint32_t soundtrack_song_id(size_t soundtrack, size_t song) {
    return static_cast<uint32_t>((soundtrack + 1) << 16 | song);
}

std::filesystem::path soundtrack_song_path(size_t soundtrack, size_t song) {
    wchar_t folder[8], file[16];
    std::swprintf(folder, 8, L"%04x", static_cast<unsigned>(soundtrack + 1));
    std::swprintf(file, 16, L"%08x.WMA", soundtrack_song_id(soundtrack, song));
    return std::filesystem::path(folder) / file;
}

std::vector<uint8_t> soundtrack_database(const std::vector<Soundtrack>& soundtracks) {
    const size_t count = std::min(soundtracks.size(), kMaxSoundtracks);
    size_t groups = 0;
    for (size_t i = 0; i < count; ++i) {
        groups += (std::min(soundtracks[i].songs.size(), kMaxSoundtrackSongs) + kGroupSongs - 1) / kGroupSongs;
    }
    std::vector<uint8_t> data((kFirstGroupBlock + groups) * kBlock, 0);
    // Header: a version, the soundtracks, the next free soundtrack id, their ids, the next free group.
    put32(data, 0, 1);
    put32(data, 4, static_cast<uint32_t>(count));
    put32(data, 8, static_cast<uint32_t>(count + 1));
    for (size_t i = 0; i < count; ++i) put32(data, 0xC + 4 * i, static_cast<uint32_t>(i + 1));
    put32(data, 0x19C, static_cast<uint32_t>(groups));
    size_t group = 0;  // group ids are global: group g is block 101 + g
    for (size_t i = 0; i < count; ++i) {
        const Soundtrack& soundtrack = soundtracks[i];
        const size_t songs = std::min(soundtrack.songs.size(), kMaxSoundtrackSongs);
        const size_t block = (i + 1) * kBlock;
        uint32_t total = 0;
        put32(data, block, kSoundtrackMagic);
        put32(data, block + 4, static_cast<uint32_t>(i + 1));
        put32(data, block + 8, static_cast<uint32_t>(songs));
        for (size_t first = 0, k = 0; first < songs; first += kGroupSongs, ++k, ++group) {
            put32(data, block + 0xC + 4 * k, static_cast<uint32_t>(group));
            const size_t g = (kFirstGroupBlock + group) * kBlock;
            put32(data, g, kGroupMagic);
            put32(data, g + 4, static_cast<uint32_t>(i + 1));
            put32(data, g + 8, static_cast<uint32_t>(k));  // the group's place in its soundtrack (song index / 6)
            put32(data, g + 0xC, static_cast<uint32_t>(group));
            for (size_t s = 0; s < kGroupSongs && first + s < songs; ++s) {
                const SoundtrackSong& song = soundtrack.songs[first + s];
                put32(data, g + 0x10 + 4 * s, soundtrack_song_id(i, first + s));
                put32(data, g + 0x28 + 4 * s, song.milliseconds);
                put_name(data, g + 0x40 + 64 * s, song.name);
                total += song.milliseconds;
            }
        }
        put32(data, block + 0x15C, total);
        put_name(data, block + 0x160, soundtrack.name);
    }
    return data;
}

std::vector<Soundtrack> parse_soundtrack_database(const std::vector<uint8_t>& data) {
    std::vector<Soundtrack> soundtracks;
    const auto u32 = [&](size_t offset) {
        uint32_t value = 0;
        if (offset + 4 <= data.size()) std::memcpy(&value, &data[offset], 4);
        return value;
    };
    const auto name = [&](size_t offset) {
        std::wstring text;
        for (size_t i = 0; i < kSoundtrackNameLength && offset + 2 * i + 2 <= data.size(); ++i) {
            wchar_t ch = 0;
            std::memcpy(&ch, &data[offset + 2 * i], 2);
            if (!ch) break;
            text += ch;
        }
        return text;
    };
    if (data.size() < kFirstGroupBlock * kBlock) return soundtracks;
    const size_t count = std::min<size_t>(u32(4), kMaxSoundtracks);
    for (size_t i = 0; i < count; ++i) {
        const size_t block = (i + 1) * kBlock;
        if (u32(block) != kSoundtrackMagic) return {};
        Soundtrack soundtrack{name(block + 0x160), {}};
        const size_t songs = std::min<size_t>(u32(block + 8), kMaxSoundtrackSongs);
        for (size_t s = 0; s < songs; ++s) {
            const size_t g = (kFirstGroupBlock + u32(block + 0xC + 4 * (s / kGroupSongs))) * kBlock;
            if (g + kBlock > data.size() || u32(g) != kGroupMagic) return {};
            soundtrack.songs.push_back({name(g + 0x40 + 64 * (s % kGroupSongs)), u32(g + 0x28 + 4 * (s % kGroupSongs))});
        }
        soundtracks.push_back(std::move(soundtrack));
    }
    return soundtracks;
}

bool convert_to_wma(const std::filesystem::path& source, const std::filesystem::path& target, uint32_t& milliseconds,
                    std::string& error) {
    const MediaFoundation mf;
    if (!mf.started) return error = "Media Foundation is not available", false;
    // The source reader decodes, resamples and mixes to the PCM asked for below (Windows 8 and later).
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(source.c_str(), nullptr, &reader))) {
        return error = "Windows cannot read " + narrow(source.filename().wstring()), false;
    }
    constexpr DWORD kAudio = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(kAudio, TRUE);
    ComPtr<IMFMediaType> pcm;
    MFCreateMediaType(&pcm);
    pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
    pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    pcm->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    pcm->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 44100 * 4);
    pcm->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (FAILED(reader->SetCurrentMediaType(kAudio, nullptr, pcm.Get()))) {
        return error = "no audio in " + narrow(source.filename().wstring()), false;
    }
    ComPtr<IMFMediaType> input;
    reader->GetCurrentMediaType(kAudio, &input);
    const ComPtr<IMFMediaType> output = wma_output_type();
    if (!output) return error = "no WMA encoder (Windows N needs the Media Feature Pack)", false;

    // The ASF sink is chosen by the file's extension: written as .wma, renamed to the target once complete.
    std::filesystem::path partial = target;
    partial += L".partial.wma";
    ComPtr<IMFSinkWriter> writer;
    DWORD stream = 0;
    if (FAILED(MFCreateSinkWriterFromURL(partial.c_str(), nullptr, nullptr, &writer)) ||
        FAILED(writer->AddStream(output.Get(), &stream)) ||
        FAILED(writer->SetInputMediaType(stream, input.Get(), nullptr)) || FAILED(writer->BeginWriting())) {
        return error = "cannot write WMA for " + narrow(source.filename().wstring()), false;
    }
    LONGLONG end = 0;
    for (;;) {
        DWORD flags = 0;
        LONGLONG time = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(kAudio, 0, nullptr, &flags, &time, &sample))) {
            error = "cannot decode " + narrow(source.filename().wstring());
            writer.Reset();
            std::error_code ignored;
            std::filesystem::remove(partial, ignored);
            return false;
        }
        if (sample) {
            LONGLONG duration = 0;
            sample->GetSampleDuration(&duration);
            end = std::max(end, time + duration);
            writer->WriteSample(stream, sample.Get());
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    if (FAILED(writer->Finalize())) return error = "cannot finish " + narrow(target.filename().wstring()), false;
    writer.Reset();
    std::error_code failed;
    std::filesystem::rename(partial, target, failed);
    if (failed) return error = "cannot rename " + narrow(partial.filename().wstring()), false;
    milliseconds = static_cast<uint32_t>(end / 10000);  // 100 ns units
    return true;
}

bool music_file(const std::filesystem::path& file) {
    std::wstring extension = file.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    for (const wchar_t* known : {L".mp3", L".flac", L".wav", L".wma", L".m4a", L".aac"}) {
        if (extension == known) return true;
    }
    return false;
}

std::vector<MusicPlaylist> load_music(const std::filesystem::path& music) {
    std::error_code error;
    const std::filesystem::path aside = with_suffix(music, L".old");
    if (!std::filesystem::exists(music, error) && std::filesystem::exists(aside, error)) {
        std::filesystem::rename(aside, music, error);  // a save stopped between its two renames
    }
    std::filesystem::remove_all(aside, error);
    std::filesystem::remove_all(with_suffix(music, L".new"), error);
    std::filesystem::remove_all(with_suffix(music, L".import"), error);
    std::ifstream in(music / L"ST.DB", std::ios::binary);
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<MusicPlaylist> playlists;
    const std::vector<Soundtrack> soundtracks = parse_soundtrack_database(data);
    for (size_t i = 0; i < soundtracks.size(); ++i) {
        MusicPlaylist playlist{soundtracks[i].name, {}};
        for (size_t s = 0; s < soundtracks[i].songs.size(); ++s) {
            const SoundtrackSong& song = soundtracks[i].songs[s];
            playlist.songs.push_back({song.name, song.milliseconds, music / soundtrack_song_path(i, s)});
        }
        playlists.push_back(std::move(playlist));
    }
    return playlists;
}

std::string save_music(const std::filesystem::path& music, const std::vector<MusicPlaylist>& playlists) {
    std::error_code error;
    const std::filesystem::path building = with_suffix(music, L".new"), aside = with_suffix(music, L".old");
    std::vector<Soundtrack> soundtracks;
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> files;  // from, to (under `building`)
    for (const MusicPlaylist& playlist : playlists) {
        if (playlist.songs.empty() || soundtracks.size() == kMaxSoundtracks) continue;
        Soundtrack soundtrack{playlist.name, {}};
        for (const MusicSong& song : playlist.songs) {
            if (soundtrack.songs.size() == kMaxSoundtrackSongs) break;
            files.emplace_back(song.file, building / soundtrack_song_path(soundtracks.size(), soundtrack.songs.size()));
            soundtrack.songs.push_back({song.name, song.milliseconds});
        }
        soundtracks.push_back(std::move(soundtrack));
    }
    if (soundtracks.empty()) {
        std::filesystem::remove_all(music, error);
        if (error) return "cannot remove the MUSIC folder: " + error.message();
        std::filesystem::remove_all(with_suffix(music, L".import"), error);
        return {};
    }
    // A hard link is instant and takes no room (the folders are side by side); a copy where the disk has none.
    std::filesystem::remove_all(building, error);
    for (const auto& [from, to] : files) {
        error.clear();
        std::filesystem::create_directories(to.parent_path(), error);
        std::filesystem::create_hard_link(from, to, error);
        if (error) {
            error.clear();
            std::filesystem::copy_file(from, to, error);
        }
        if (error) {
            const std::string why = "cannot keep " + narrow(from.filename().wstring()) + ": " + error.message();
            std::filesystem::remove_all(building, error);
            return why;
        }
    }
    const std::vector<uint8_t> database = soundtrack_database(soundtracks);
    {
        std::ofstream out(building / L"ST.DB", std::ios::binary);
        out.write(reinterpret_cast<const char*>(database.data()), static_cast<std::streamsize>(database.size()));
        if (!out) {
            out.close();
            std::filesystem::remove_all(building, error);
            return "cannot write ST.DB";
        }
    }
    std::filesystem::remove_all(aside, error);
    error.clear();
    if (std::filesystem::exists(music, error)) std::filesystem::rename(music, aside, error);
    if (error) {
        const std::string why = "cannot replace the MUSIC folder: " + error.message();
        std::filesystem::remove_all(building, error);
        return why;
    }
    std::filesystem::rename(building, music, error);
    if (error) {
        const std::string why = "cannot replace the MUSIC folder: " + error.message();
        std::filesystem::rename(aside, music, error);
        return why;
    }
    std::filesystem::remove_all(aside, error);
    std::filesystem::remove_all(with_suffix(music, L".import"), error);
    return {};
}

MusicImport import_songs(const std::vector<std::filesystem::path>& files, const std::filesystem::path& music,
                         MusicPlaylist& playlist, const std::function<bool(size_t, size_t, const std::wstring&)>& progress) {
    MusicImport result;
    const std::filesystem::path staging = with_suffix(music, L".import");
    std::error_code error;
    std::filesystem::create_directories(staging, error);
    size_t next = 0;
    for (size_t done = 0; done < files.size(); ++done) {
        const std::filesystem::path& file = files[done];
        if (progress && !progress(done, files.size(), file.filename().wstring())) break;
        const auto fail = [&](const std::string& why) {
            ++result.failed;
            if (result.error.empty()) result.error = why;
        };
        if (playlist.songs.size() >= kMaxSoundtrackSongs) {
            fail("a playlist holds at most 504 songs");
            continue;
        }
        if (!music_file(file)) {
            fail("not an audio file: " + narrow(file.filename().wstring()));
            continue;
        }
        std::filesystem::path target;
        do {
            target = staging / (std::to_wstring(next++) + L".WMA");
        } while (std::filesystem::exists(target, error));
        uint32_t milliseconds = 0;
        std::string why;
        if (!convert_to_wma(file, target, milliseconds, why) || !milliseconds) {
            fail(why.empty() ? "no sound in " + narrow(file.filename().wstring()) : why);
            std::filesystem::remove(target, error);
            continue;
        }
        playlist.songs.push_back({song_name(file), milliseconds, target});
        ++result.songs;
    }
    return result;
}

std::vector<std::pair<std::wstring, std::vector<std::filesystem::path>>> folder_playlists(const std::filesystem::path& source) {
    std::vector<std::pair<std::wstring, std::vector<std::filesystem::path>>> playlists;
    if (auto files = audio_files(source); !files.empty()) playlists.emplace_back(source.filename().wstring(), std::move(files));
    std::error_code error;
    std::vector<std::filesystem::path> folders;
    for (const auto& entry : std::filesystem::directory_iterator(source, error)) {
        if (entry.is_directory(error)) folders.push_back(entry.path());
    }
    std::sort(folders.begin(), folders.end());
    for (const auto& folder : folders) {
        if (auto files = audio_files(folder); !files.empty()) playlists.emplace_back(folder.filename().wstring(), std::move(files));
    }
    return playlists;
}

}  // namespace xport::pc
