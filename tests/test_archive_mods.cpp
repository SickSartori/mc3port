// Entries of the game's Dave archives replaced by files of the mods folder (archive_mods.h): the names as the game
// decodes them, the rewritten directory, and reads across the directory, the original data and the appended files.

#include <cstring>
#include <string>
#include <vector>

#include "pc/archive_mods.h"
#include "test.h"

using namespace xport::pc;

namespace {

constexpr char kAlphabet[] = "\0 #$()-./?0123456789_abcdefghijklmnopqrstuvwxyz~";

uint32_t u32(const std::vector<uint8_t>& data, size_t offset) {
    uint32_t value;
    std::memcpy(&value, &data[offset], 4);
    return value;
}

void put32(std::vector<uint8_t>& data, size_t offset, uint32_t value) { std::memcpy(&data[offset], &value, 4); }

// Packs symbols four to three bytes, the lowest six bits first.
void pack(std::vector<uint8_t>& names, std::vector<uint8_t> symbols) {
    while (symbols.size() % 4) symbols.push_back(0);
    for (size_t i = 0; i < symbols.size(); i += 4) {
        const uint32_t group = symbols[i] | symbols[i + 1] << 6 | symbols[i + 2] << 12 | symbols[i + 3] << 18;
        names.push_back(static_cast<uint8_t>(group)), names.push_back(static_cast<uint8_t>(group >> 8)),
            names.push_back(static_cast<uint8_t>(group >> 16));
    }
}

std::vector<uint8_t> symbols_of(const std::string& text) {
    std::vector<uint8_t> symbols;
    for (char c : text) symbols.push_back(static_cast<uint8_t>(std::string(kAlphabet + 1, sizeof(kAlphabet) - 2).find(c) + 1));
    symbols.push_back(0);
    return symbols;
}

struct Entry {
    std::vector<uint8_t> name_symbols;
    std::vector<uint8_t> data;
};

// An archive like the game's: header, directory at 0x800, names after it, each entry's data 2048-aligned.
std::vector<uint8_t> make_archive(const std::vector<Entry>& entries) {
    std::vector<uint8_t> names;
    std::vector<uint32_t> name_offsets;
    for (const Entry& entry : entries) name_offsets.push_back(static_cast<uint32_t>(names.size())), pack(names, entry.name_symbols);
    const uint32_t directory = static_cast<uint32_t>(entries.size() * 16);
    std::vector<uint8_t> archive(0x800 + directory);
    std::memcpy(archive.data(), "Dave", 4);
    put32(archive, 4, static_cast<uint32_t>(entries.size())), put32(archive, 8, directory),
        put32(archive, 12, static_cast<uint32_t>(names.size()));
    archive.insert(archive.end(), names.begin(), names.end());
    for (size_t i = 0; i < entries.size(); ++i) {
        archive.resize((archive.size() + 2047) & ~size_t{2047});
        const size_t record = 0x800 + i * 16;
        put32(archive, record, name_offsets[i]), put32(archive, record + 4, static_cast<uint32_t>(archive.size()));
        put32(archive, record + 8, static_cast<uint32_t>(entries[i].data.size())),
            put32(archive, record + 12, static_cast<uint32_t>(entries[i].data.size()));
        archive.insert(archive.end(), entries[i].data.begin(), entries[i].data.end());
    }
    return archive;
}

std::vector<uint8_t> bytes(const std::string& text) { return {text.begin(), text.end()}; }

}  // namespace

int main() {
    // tune/a.txt; tune/b.txt sharing "tune/" (5 characters: 0x38 + 5, then a symbol with 0 in its low five bits);
    // a name with symbol 48, which the game ends there; a directory entry; a name that repeats.
    std::vector<uint8_t> shared = {0x38 + 5, 0x20};
    for (uint8_t s : symbols_of("b.txt")) shared.push_back(s);
    std::vector<uint8_t> odd = symbols_of("anim/x");
    odd.pop_back();
    odd.push_back(48);
    for (uint8_t s : symbols_of("12.anim")) odd.push_back(s);
    const std::vector<Entry> entries = {
        {symbols_of("tune/a.txt"), bytes("original a")},
        {shared, bytes("original b, longer")},
        {odd, bytes("animation")},
        {symbols_of("dir/"), {}},
        {symbols_of("tune/a.txt"), bytes("original a")},
    };
    const std::vector<uint8_t> archive = make_archive(entries);
    const ArchiveRead base = [&](uint64_t offset, void* destination, size_t size) {
        if (offset >= archive.size()) return size_t{0};
        size = std::min<size_t>(size, archive.size() - static_cast<size_t>(offset));
        std::memcpy(destination, archive.data() + offset, size);
        return size;
    };

    std::vector<uint8_t> head(archive.begin(), archive.begin() + 0x800 + 5 * 16 + u32(archive, 12));
    const std::vector<std::string> names = dave_entry_names(head);
    CHECK(names.size() == 5);
    if (names.size() == 5) {
        CHECK(names[0] == "tune/a.txt");
        CHECK(names[1] == "tune/b.txt");
        CHECK(names[2] == "anim/x");
        CHECK(names[3] == "dir/");
        CHECK(names[4] == "tune/a.txt");
    }
    CHECK(dave_entry_names(std::vector<uint8_t>(16)).empty());

    TemporaryFolder folder("mc3port_test_archive_mods");
    std::filesystem::create_directories(folder.path / "Tune");
    const std::string mod_a = "modded a, a good deal longer than the original";
    write_file(folder.path / "Tune" / "A.TXT", mod_a.data(), mod_a.size());
    write_file(folder.path / "unrelated.bin", "x", 1);
    const auto mods = mod_files(folder.path);
    CHECK(mods.count("tune/a.txt") == 1);
    CHECK(mods.count("unrelated.bin") == 1);

    CHECK(make_archive_overlay(base, archive.size(), {}) == nullptr);
    const auto overlay = make_archive_overlay(base, archive.size(), mods);
    CHECK(overlay != nullptr);
    if (!overlay) return test_result();
    CHECK(overlay->replaced.size() == 1);
    CHECK(overlay->pieces.size() == 1);
    const uint64_t at = (archive.size() + 2047) & ~size_t{2047};
    CHECK(overlay->pieces[0].offset == at);
    CHECK(overlay->size == at + mod_a.size());

    // The whole archive as the game sees it.
    std::vector<uint8_t> seen(static_cast<size_t>(overlay->size));
    CHECK(read_archive_overlay(*overlay, base, 0, seen.data(), seen.size()) == seen.size());
    for (size_t i : {size_t{0}, size_t{4}}) {  // both copies of the name point to the mod, uncompressed
        CHECK(u32(seen, 0x800 + i * 16 + 4) == at);
        CHECK(u32(seen, 0x800 + i * 16 + 8) == mod_a.size());
        CHECK(u32(seen, 0x800 + i * 16 + 12) == mod_a.size());
        CHECK(u32(seen, 0x800 + i * 16) == u32(archive, 0x800 + i * 16));  // the name stays
    }
    CHECK(std::memcmp(seen.data() + 0x800 + 16, archive.data() + 0x800 + 16, 3 * 16) == 0);  // other records
    CHECK(std::memcmp(seen.data() + 0x800 + 5 * 16, archive.data() + 0x800 + 5 * 16, archive.size() - 0x800 - 5 * 16) == 0);
    CHECK(std::string(seen.begin() + static_cast<ptrdiff_t>(at), seen.end()) == mod_a);
    CHECK(seen[archive.size()] == 0);  // the alignment gap

    // Reads across the borders give the same bytes as the whole.
    for (const auto& [offset, size] : std::vector<std::pair<uint64_t, size_t>>{
             {0x7F0, 0x40}, {archive.size() - 3, 2048 + 8}, {at - 1, 3}, {at + 5, 1000}, {overlay->size, 4}}) {
        std::vector<uint8_t> part(size, 0xCC);
        const size_t read = read_archive_overlay(*overlay, base, offset, part.data(), size);
        const size_t expected = offset >= overlay->size ? 0 : std::min<size_t>(size, static_cast<size_t>(overlay->size - offset));
        CHECK(read == expected);
        if (read == expected && read) CHECK(std::memcmp(part.data(), seen.data() + offset, read) == 0);
    }

    // Not a Dave archive: nothing to do.
    std::vector<uint8_t> other(0x1000, 0);
    const ArchiveRead other_base = [&](uint64_t offset, void* destination, size_t size) {
        std::memcpy(destination, other.data() + offset, size);
        return size;
    };
    CHECK(make_archive_overlay(other_base, other.size(), mods) == nullptr);
    return test_result();
}
