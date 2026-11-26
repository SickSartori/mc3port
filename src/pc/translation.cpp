// game_language 7, Português (Brasil): João13 e cia's translation of the Xbox game (joao13traducoes.com, CC BY-NC 4.0),
// added to mc3port at the suggestion of NAKADANI's video. It replaces the game's English: 588 files of the disc (the
// text archives, the menus' and loading screens' Flash files, the radar's map). mc3port ships them as differences from
// the disc's files (translations\pt-br.mc3patch, 2.6 MB, made by sick-xport's tools/translation/make_patch.py), and
// makes the files once, from the player's own disc, in the data folder: the European and the USA discs have the same
// files (only their XBEs differ), so both work.
//
// pt-br.mc3patch, little-endian: "MC3PATCH", u32 version 1, u32 count, then per file: u16 name length, name (UTF-8,
// '/'), u32 source size, 20-byte source SHA-1, u32 target size, 20-byte target SHA-1, u32 delta size, VCDIFF delta.

#include "pc/translation.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/xdvdfs.h"
#include "pc/log.h"
#include "pc/paths.h"
#include "pc/settings.h"
#include "pc/vcdiff.h"

namespace xport::pc {
namespace {

namespace fs = std::filesystem;

fs::path g_folder;

struct Sha1 {
    BCRYPT_HASH_HANDLE handle = nullptr;
    Sha1() { BCryptCreateHash(BCRYPT_SHA1_ALG_HANDLE, &handle, nullptr, 0, nullptr, 0, 0); }
    ~Sha1() {
        if (handle) BCryptDestroyHash(handle);
    }
    void add(const uint8_t* data, size_t size) { BCryptHashData(handle, const_cast<uint8_t*>(data), static_cast<ULONG>(size), 0); }
    std::string finish() {
        uint8_t digest[20] = {};
        BCryptFinishHash(handle, digest, sizeof(digest), 0);
        return std::string(reinterpret_cast<const char*>(digest), sizeof(digest));
    }
};

struct Entry {
    std::string name;
    uint32_t source_size = 0, target_size = 0, delta_size = 0;
    std::string source_sha1, target_sha1;
    const uint8_t* delta = nullptr;
};

bool parse(const std::vector<uint8_t>& patch, std::vector<Entry>& entries) {
    size_t at = 0;
    const auto take = [&](void* out, size_t size) {
        if (at + size > patch.size()) return false;
        std::memcpy(out, patch.data() + at, size);
        at += size;
        return true;
    };
    char magic[8];
    uint32_t version = 0, count = 0;
    if (!take(magic, 8) || std::memcmp(magic, "MC3PATCH", 8) != 0 || !take(&version, 4) || version != 1 ||
        !take(&count, 4)) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        Entry e;
        uint16_t length = 0;
        char sha[20];
        if (!take(&length, 2) || at + length > patch.size()) return false;
        e.name.assign(reinterpret_cast<const char*>(patch.data() + at), length);
        at += length;
        if (!take(&e.source_size, 4) || !take(sha, 20)) return false;
        e.source_sha1.assign(sha, 20);
        if (!take(&e.target_size, 4) || !take(sha, 20) || !take(&e.delta_size, 4)) return false;
        e.target_sha1.assign(sha, 20);
        if (at + e.delta_size > patch.size()) return false;
        e.delta = patch.data() + at;
        at += e.delta_size;
        entries.push_back(std::move(e));
    }
    return at == patch.size();
}

// The file made from the disc's, as <name>.part until its SHA-1 is right, then under its name.
std::string make_file(const XdvdfsImage& disc, const Entry& e, const fs::path& target) {
    const std::optional<XdvdfsImage::Entry> source = disc.find(e.name);
    if (!source || source->size != e.source_size) return "the disc has no " + e.name + " of the expected size";
    std::error_code error;
    fs::create_directories(target.parent_path(), error);
    fs::path part = target;
    part += ".part";
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    if (!out) return "cannot write " + part.string();
    Sha1 sha;
    std::string why;
    const bool ok = vcdiff_decode(
        e.delta, e.delta_size,
        [&](uint64_t offset, uint8_t* data, size_t size) { return disc.read(*source, offset, data, size) == size; },
        [&](const uint8_t* data, size_t size) {
            sha.add(data, size);
            out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
            return static_cast<bool>(out);
        },
        &why);
    const std::streamoff size = out.tellp();
    out.close();
    if (!ok || size != e.target_size || sha.finish() != e.target_sha1) {
        fs::remove(part, error);
        return e.name + (ok ? ": the result differs (another version of the game?)" : ": " + why);
    }
    fs::rename(part, target, error);
    return error ? "cannot rename " + part.string() : "";
}

}  // namespace

const fs::path& translation_folder() { return g_folder; }

void translation_prepare(const XdvdfsImage& disc) {
    if (static_cast<int>(setting_at_start("game_language")) != kLanguagePortuguese) return;
    // The translation is over the game's English: the game is told English (xport's kernel reads MC3_LANGUAGE first).
    if (!GetEnvironmentVariableA("MC3_LANGUAGE", nullptr, 0)) SetEnvironmentVariableA("MC3_LANGUAGE", "1");
    const fs::path patch_path = program_directory() / "translations" / "pt-br.mc3patch";
    const fs::path folder = data_directory() / "translations" / "pt-br";
    const auto give_up = [&](const std::string& why) {
        log_line("translation: %s; the game is in English", why.c_str());
        const std::wstring text =
            L"The Portuguese (Brazil) translation could not be prepared, so the game starts in English.\n\n" +
            std::wstring(why.begin(), why.end());
        MessageBoxW(nullptr, text.c_str(), L"mc3port", MB_ICONWARNING | MB_OK);
    };
    std::ifstream in(patch_path, std::ios::binary);
    const std::vector<uint8_t> patch((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<Entry> entries;
    if (patch.empty() || !parse(patch, entries)) return give_up("translations\\pt-br.mc3patch is missing or damaged");

    // A start after the first: every file is there (each was checked before it got its name), for this patch.
    Sha1 patch_sha;
    patch_sha.add(patch.data(), patch.size());
    char stamp[48];
    const std::string digest = patch_sha.finish();
    for (int i = 0; i < 20; ++i) std::snprintf(stamp + 2 * i, 3, "%02x", static_cast<uint8_t>(digest[i]));
    const fs::path done = folder / "done.txt";
    {
        std::ifstream marker(done);
        std::string line;
        if (marker && std::getline(marker, line) && line == stamp) {
            g_folder = folder;
            log_line("translation: Português (Brasil), %zu files in %s", entries.size(), folder.string().c_str());
            return;
        }
        marker.close();
        std::error_code ignored;
        if (!line.empty()) fs::remove_all(folder, ignored);  // made from another version of the patch
    }

    uint64_t needed = 0;
    std::error_code error;
    for (const Entry& e : entries) {
        const fs::path target = folder / fs::u8path(e.name);
        if (!(fs::file_size(target, error) == e.target_size && !error)) needed += e.target_size;
    }
    fs::create_directories(folder, error);
    const fs::space_info space = fs::space(folder, error);
    if (!error && space.available < needed + (64ull << 20)) {
        return give_up("not enough free space: " + std::to_string((needed >> 20) + 64) + " MB needed in " + folder.string());
    }
    log_line("translation: making %llu MB of translated files from the disc in %s",
             static_cast<unsigned long long>(needed >> 20), folder.string().c_str());
    const ULONGLONG start = GetTickCount64();
    for (const Entry& e : entries) {
        const fs::path target = folder / fs::u8path(e.name);
        if (fs::file_size(target, error) == e.target_size && !error) continue;  // made by an earlier, cut start
        const std::string why = make_file(disc, e, target);
        if (!why.empty()) return give_up(why);
    }
    std::ofstream(done) << stamp << "\n";
    g_folder = folder;
    log_line("translation: Português (Brasil) ready in %.1f s", (GetTickCount64() - start) / 1000.0);
}

}  // namespace xport::pc
