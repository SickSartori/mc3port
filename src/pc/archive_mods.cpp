// The Dave archive, as MC3's XBE reads it (sick-xport INDIRIZZI-XBE.md §10, MODS-FORMATI-PS2.md §6b): "Dave", u32
// entry count, u32 directory size, u32 name table size; at 0x800 the directory, 16 bytes an entry {name offset, data
// offset, full size, stored size} (stored < full: raw deflate); after it the names, 6-bit symbols over a 48-symbol
// alphabet (0x4B3B48), four to three bytes, a symbol of 0x38 or more first reusing a prefix of the previous name.

#include "pc/archive_mods.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include "pc/log.h"

namespace xport::pc {
namespace fs = std::filesystem;

namespace {

constexpr char kAlphabet[] = "\0 #$()-./?0123456789_abcdefghijklmnopqrstuvwxyz~";
constexpr size_t kSymbols = sizeof(kAlphabet) - 1;  // 48: the byte after the game's table is 0
constexpr uint32_t kDirectory = 0x800;
constexpr uint64_t kAlignment = 2048;

uint32_t u32(const std::vector<uint8_t>& data, size_t offset) {
    uint32_t value;
    std::memcpy(&value, &data[offset], 4);
    return value;
}

bool dave_header(const std::vector<uint8_t>& head, uint32_t* count, uint32_t* directory, uint32_t* names) {
    if (head.size() < 16 || std::memcmp(head.data(), "Dave", 4) != 0) return false;
    *count = u32(head, 4), *directory = u32(head, 8), *names = u32(head, 12);
    return uint64_t{*count} * 16 <= *directory && head.size() >= uint64_t{kDirectory} + *directory + *names;
}

}  // namespace

ArchiveOverlay::~ArchiveOverlay() {
    for (const Piece& piece : pieces) {
        if (piece.file != INVALID_HANDLE_VALUE) CloseHandle(piece.file);
    }
}

std::vector<std::string> dave_entry_names(const std::vector<uint8_t>& head) {
    uint32_t count, directory, names_size;
    if (!dave_header(head, &count, &directory, &names_size)) return {};
    const uint8_t* names = head.data() + kDirectory + directory;
    std::vector<std::string> result;
    result.reserve(count);
    std::string previous;  // as the game keeps it: past a 0 from symbol 48 too, for the next name's shared prefix
    for (uint32_t i = 0; i < count; ++i) {
        size_t at = u32(head, kDirectory + i * 16);
        uint32_t group = 0;
        int used = 4;  // symbols of `group` taken, lowest six bits first
        const auto next = [&](uint8_t* symbol) {
            if (used == 4) {
                if (at + 3 > names_size) return false;
                group = names[at] | names[at + 1] << 8 | names[at + 2] << 16;
                at += 3, used = 0;
            }
            *symbol = static_cast<uint8_t>(group >> (6 * used++) & 0x3F);
            return true;
        };
        std::string name;
        uint8_t symbol;
        if (!next(&symbol)) return {};
        if (symbol >= 0x38) {
            uint8_t high;
            if (!next(&high)) return {};
            name = previous.substr(0, std::min<size_t>(previous.size(), (symbol - 0x38) + ((high & 0x1F) << 3)));
            if (!next(&symbol)) return {};
        }
        while (symbol) {
            name += symbol < kSymbols ? kAlphabet[symbol] : '\0';
            if (!next(&symbol)) return {};
        }
        previous = name;
        result.push_back(name.substr(0, name.find('\0')));
    }
    return result;
}

std::map<std::string, fs::path> mod_files(const fs::path& folder) {
    std::map<std::string, fs::path> files;
    std::error_code error;
    for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error)) continue;
        std::string key = fs::relative(it->path(), folder, error).generic_string();
        std::transform(key.begin(), key.end(), key.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; });
        files.emplace(std::move(key), it->path());
    }
    return files;
}

namespace {

// A name the game can find: the alphabet's symbols but '?' (the game compares names as text and the packed symbols in
// different orders only for it), not a folder.
bool addable(const std::string& name) {
    if (name.empty() || name.back() == '/' || name.size() > 255) return false;
    for (char c : name) {
        if (c == '?' || c == '\0' || !std::memchr(kAlphabet + 1, c, kSymbols - 1)) return false;
    }
    return true;
}

// The names after the directory, each from a byte of its own, sharing the previous name's beginning when they can.
std::vector<uint8_t> encode_names(const std::vector<std::string>& names, std::vector<uint32_t>& offsets) {
    std::vector<uint8_t> out;
    const std::string* previous = nullptr;
    for (const std::string& name : names) {
        offsets.push_back(static_cast<uint32_t>(out.size()));
        size_t shared = 0;
        if (previous) {
            while (shared < 255 && shared < name.size() && shared < previous->size() && name[shared] == (*previous)[shared]) {
                ++shared;
            }
        }
        std::vector<uint8_t> symbols;
        if (shared) symbols = {static_cast<uint8_t>(0x38 + (shared & 7)), static_cast<uint8_t>(0x20 + (shared >> 3))};
        for (size_t i = shared; i < name.size(); ++i) {
            const char* at = static_cast<const char*>(std::memchr(kAlphabet + 1, name[i], kSymbols - 1));
            symbols.push_back(static_cast<uint8_t>(at - kAlphabet));
        }
        symbols.push_back(0);
        while (symbols.size() % 4) symbols.push_back(0);
        for (size_t i = 0; i < symbols.size(); i += 4) {
            const uint32_t group = symbols[i] | symbols[i + 1] << 6 | symbols[i + 2] << 12 | symbols[i + 3] << 18;
            out.push_back(static_cast<uint8_t>(group)), out.push_back(static_cast<uint8_t>(group >> 8)),
                out.push_back(static_cast<uint8_t>(group >> 16));
        }
        previous = &name;
    }
    return out;
}

uint64_t aligned(uint64_t value, uint64_t to) { return (value + to - 1) & ~(to - 1); }

}  // namespace

std::shared_ptr<const ArchiveOverlay> make_archive_overlay(const ArchiveRead& base, uint64_t base_size,
                                                           const std::map<std::string, fs::path>& mods,
                                                           const std::vector<std::string>& additions) {
    if (mods.empty() || base_size < kDirectory) return nullptr;
    std::vector<uint8_t> head(16);
    if (base(0, head.data(), head.size()) != head.size()) return nullptr;
    if (std::memcmp(head.data(), "Dave", 4) != 0) return nullptr;
    const uint32_t count = u32(head, 4), directory = u32(head, 8), names_size = u32(head, 12);
    const uint64_t head_size = uint64_t{kDirectory} + directory + names_size;
    if (head_size > base_size || uint64_t{count} * 16 > directory) return nullptr;
    head.resize(static_cast<size_t>(head_size));
    if (base(0, head.data(), head.size()) != head.size()) return nullptr;
    const std::vector<std::string> names = dave_entry_names(head);
    if (names.size() != count) return nullptr;

    auto overlay = std::make_shared<ArchiveOverlay>();
    overlay->base_size = base_size;
    std::unordered_map<std::string, size_t> piece_of;  // one copy of a file a name that repeats points to
    const auto open_piece = [&](const std::string& name, const fs::path& path, uint32_t original) -> int {
        if (const auto known = piece_of.find(name); known != piece_of.end()) return static_cast<int>(known->second);
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        LARGE_INTEGER size{};
        if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &size) || size.QuadPart > 0x7FFFFFFF) {
            log_line("mods: cannot read %s (error %lu)", path.string().c_str(), GetLastError());
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            return -1;
        }
        if (original && size.QuadPart > original && name.starts_with("resources/vehicle/")) {
            // The game keeps a car's parts in blocks of fixed size: a bigger file may not fit (znxee had to grow them).
            log_line("mods: %s is bigger than the game's (%lld bytes, the game's %u): a car may not load", name.c_str(),
                     size.QuadPart, original);
        }
        overlay->pieces.push_back({0, static_cast<uint32_t>(size.QuadPart), file});
        overlay->replaced.push_back(name);
        piece_of.emplace(name, overlay->pieces.size() - 1);
        return static_cast<int>(overlay->pieces.size() - 1);
    };

    struct Record {
        std::string name;
        uint32_t fields[3];  // data offset, full size, stored size
        int piece;
    };
    std::vector<Record> records;
    records.reserve(count + additions.size());
    for (uint32_t i = 0; i < count; ++i) {
        const size_t record = kDirectory + i * 16;
        Record r{names[i], {u32(head, record + 4), u32(head, record + 8), u32(head, record + 12)}, -1};
        const auto mod = mods.find(names[i]);
        if (mod != mods.end() && (r.fields[1] || r.fields[2])) r.piece = open_piece(names[i], mod->second, r.fields[1]);
        records.push_back(std::move(r));
    }
    // Added files, each with the folders it needs that the archive lacks.
    std::unordered_set<std::string> present(names.begin(), names.end());
    size_t added = 0;
    for (const std::string& name : additions) {
        const auto mod = mods.find(name);
        if (mod == mods.end() || present.count(name)) continue;
        if (!addable(name)) {
            log_line("mods: %s cannot be added to an archive (a character the game's names do not have)", name.c_str());
            continue;
        }
        const int piece = open_piece(name, mod->second, 0);
        if (piece < 0) continue;
        for (size_t slash = name.find('/'); slash != std::string::npos; slash = name.find('/', slash + 1)) {
            const std::string folder = name.substr(0, slash + 1);
            if (present.insert(folder).second) records.push_back({folder, {0, 0, 0}, -1});
        }
        present.insert(name);
        records.push_back({name, {0, 0, 0}, piece});
        ++added;
    }
    if (overlay->pieces.empty()) return nullptr;

    // The game finds a name by binary search over the directory (0x2B35E0, bsearch at 0x2DD0DD), comparing as text
    // without case (0x2B2DD0): added names go in order, the directory and the names are written again, and the
    // archive's own data moves further on by what they grew.
    uint64_t delta = 0;
    if (added) {
        std::stable_sort(records.begin(), records.end(), [](const Record& a, const Record& b) { return a.name < b.name; });
        std::vector<std::string> sorted;
        sorted.reserve(records.size());
        for (const Record& r : records) sorted.push_back(r.name);
        std::vector<uint32_t> offsets;
        const std::vector<uint8_t> packed = encode_names(sorted, offsets);
        const uint64_t new_directory = aligned(records.size() * 16, kDirectory);
        const uint64_t new_names = aligned(packed.size(), kDirectory);
        const uint64_t new_head = kDirectory + new_directory + new_names;
        if (new_head > head_size) delta = aligned(new_head - head_size, kAlignment);
        std::vector<uint8_t> rebuilt(static_cast<size_t>(head_size + delta), 0);
        std::memcpy(rebuilt.data(), head.data(), 4);
        const uint32_t header[3] = {static_cast<uint32_t>(records.size()), static_cast<uint32_t>(new_directory),
                                    static_cast<uint32_t>(new_names)};
        std::memcpy(&rebuilt[4], header, sizeof(header));
        std::memcpy(&rebuilt[static_cast<size_t>(kDirectory + new_directory)], packed.data(), packed.size());
        for (size_t i = 0; i < records.size(); ++i) {
            Record& r = records[i];
            if (r.piece < 0 && (r.fields[1] || r.fields[2])) r.fields[0] += static_cast<uint32_t>(delta);
            std::memcpy(&rebuilt[kDirectory + i * 16], &offsets[i], 4);
        }
        head = std::move(rebuilt);
        log_line("mods: %zu entries added, the archive's directory written again (%zu entries, its data %llu bytes further on)",
                 added, records.size(), static_cast<unsigned long long>(delta));
    }
    uint64_t end = base_size + delta;
    for (ArchiveOverlay::Piece& piece : overlay->pieces) {
        piece.offset = aligned(end, kAlignment);
        end = piece.offset + piece.size;
    }
    if (end > 0xFFFFFFFFull) {  // the directory's offsets are 32-bit
        log_line("mods: the archive with its mods would pass 4 GB: no mods for it");
        return nullptr;
    }
    for (size_t i = 0; i < records.size(); ++i) {
        Record& r = records[i];
        if (r.piece >= 0) {
            const ArchiveOverlay::Piece& piece = overlay->pieces[static_cast<size_t>(r.piece)];
            r.fields[0] = static_cast<uint32_t>(piece.offset), r.fields[1] = r.fields[2] = piece.size;  // stored = full: raw
        }
        std::memcpy(&head[kDirectory + i * 16 + 4], r.fields, sizeof(r.fields));
    }
    overlay->head = std::move(head);
    overlay->delta = delta;
    overlay->read_once = std::make_unique<std::atomic<bool>[]>(overlay->pieces.size());
    overlay->size = end;
    return overlay;
}

size_t read_archive_overlay(const ArchiveOverlay& overlay, const ArchiveRead& base, uint64_t offset, void* destination,
                            size_t size) {
    if (offset >= overlay.size) return 0;
    size = static_cast<size_t>(std::min<uint64_t>(size, overlay.size - offset));
    uint8_t* out = static_cast<uint8_t*>(destination);
    size_t done = 0;
    if (offset < overlay.head.size()) {  // the header, the directory and the names, written again
        done = static_cast<size_t>(std::min<uint64_t>(size, overlay.head.size() - offset));
        std::memcpy(out, overlay.head.data() + offset, done);
    }
    const uint64_t data_end = overlay.base_size + overlay.delta;
    if (done < size && offset + done < data_end) {  // the archive's own data, moved by delta
        const size_t part = static_cast<size_t>(std::min<uint64_t>(size - done, data_end - (offset + done)));
        const size_t read = base(offset + done - overlay.delta, out + done, part);
        done += read;
        if (read < part) return done;
    }
    while (done < size) {
        const uint64_t at = offset + done;
        const auto piece = std::upper_bound(overlay.pieces.begin(), overlay.pieces.end(), at,
                                            [](uint64_t value, const ArchiveOverlay::Piece& p) { return value < p.offset; });
        size_t part;
        if (piece != overlay.pieces.begin() && at < (piece - 1)->offset + (piece - 1)->size) {
            const ArchiveOverlay::Piece& p = *(piece - 1);
            const size_t index = static_cast<size_t>(piece - 1 - overlay.pieces.begin());
            if (overlay.read_once && !overlay.read_once[index].exchange(true)) {
                log_line("mods: the game reads %s", overlay.replaced[index].c_str());
            }
            part = static_cast<size_t>(std::min<uint64_t>(size - done, p.offset + p.size - at));
            OVERLAPPED overlapped{};
            overlapped.Offset = static_cast<DWORD>(at - p.offset);
            DWORD count = 0;
            if (!ReadFile(p.file, out + done, static_cast<DWORD>(part), &count, &overlapped) || count != part) {
                return done + count;
            }
        } else {  // the alignment gap before a file
            const uint64_t gap_end = piece != overlay.pieces.end() ? piece->offset : overlay.size;
            part = static_cast<size_t>(std::min<uint64_t>(size - done, gap_end - at));
            std::memset(out + done, 0, part);
        }
        done += part;
    }
    return done;
}

std::vector<std::vector<std::string>> assign_additions(const std::vector<std::vector<std::string>>& archives,
                                                       const std::map<std::string, fs::path>& mods,
                                                       const std::function<bool(const std::string&)>& on_disc) {
    std::vector<std::unordered_set<std::string>> entries(archives.size()), folders(archives.size());
    for (size_t a = 0; a < archives.size(); ++a) {
        for (const std::string& name : archives[a]) {
            entries[a].insert(name);
            for (size_t slash = name.find('/'); slash != std::string::npos; slash = name.find('/', slash + 1)) {
                folders[a].insert(name.substr(0, slash + 1));
            }
        }
    }
    std::vector<std::vector<std::string>> result(archives.size());
    for (const auto& [name, path] : mods) {
        if (name.find('/') == std::string::npos || on_disc(name)) continue;
        bool known = false;
        for (const auto& set : entries) known = known || set.count(name);
        if (known) continue;
        size_t best = 0, best_depth = 0;
        for (size_t a = 0; a < archives.size(); ++a) {
            size_t depth = 0;
            for (size_t slash = name.find('/'); slash != std::string::npos; slash = name.find('/', slash + 1)) {
                if (!folders[a].count(name.substr(0, slash + 1))) break;
                ++depth;
            }
            if (depth > best_depth) best = a, best_depth = depth;
        }
        if (best_depth) result[best].push_back(name);
    }
    return result;
}

}  // namespace xport::pc
