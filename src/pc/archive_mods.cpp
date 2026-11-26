// The Dave archive, as MC3's XBE reads it (sick-xport INDIRIZZI-XBE.md §10, MODS-FORMATI-PS2.md §6b): "Dave", u32
// entry count, u32 directory size, u32 name table size; at 0x800 the directory, 16 bytes an entry {name offset, data
// offset, full size, stored size} (stored < full: raw deflate); after it the names, 6-bit symbols over a 48-symbol
// alphabet (0x4B3B48), four to three bytes, a symbol of 0x38 or more first reusing a prefix of the previous name.

#include "pc/archive_mods.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

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

std::shared_ptr<const ArchiveOverlay> make_archive_overlay(const ArchiveRead& base, uint64_t base_size,
                                                           const std::map<std::string, fs::path>& mods) {
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
    uint64_t end = base_size;
    std::unordered_map<std::string, size_t> piece_of;  // one copy of a file a name that repeats points to
    for (uint32_t i = 0; i < count; ++i) {
        const auto mod = mods.find(names[i]);
        const size_t record = kDirectory + i * 16;
        if (mod == mods.end() || (u32(head, record + 8) == 0 && u32(head, record + 12) == 0)) continue;
        auto known = piece_of.find(names[i]);
        if (known == piece_of.end()) {
            const HANDLE file = CreateFileW(mod->second.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                            FILE_ATTRIBUTE_NORMAL, nullptr);
            LARGE_INTEGER size{};
            if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &size)) {
                log_line("mods: cannot read %s (error %lu): the archive's own stays", mod->second.string().c_str(),
                         GetLastError());
                if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
                continue;
            }
            const uint64_t at = (end + kAlignment - 1) & ~(kAlignment - 1);
            if (at + static_cast<uint64_t>(size.QuadPart) > 0xFFFFFFFFull) {  // the directory's offsets are 32-bit
                log_line("mods: %s does not fit after the archive's 4 GB: the archive's own stays", names[i].c_str());
                CloseHandle(file);
                continue;
            }
            overlay->pieces.push_back({at, static_cast<uint32_t>(size.QuadPart), file});
            end = at + static_cast<uint64_t>(size.QuadPart);
            known = piece_of.emplace(names[i], overlay->pieces.size() - 1).first;
            overlay->replaced.push_back(names[i]);
            const uint32_t original = u32(head, record + 8);
            if (size.QuadPart > original && names[i].starts_with("resources/vehicle/")) {
                // The game keeps a car's parts in blocks of fixed size: a bigger file may not fit (znxee had to grow them).
                log_line("mods: %s is bigger than the game's (%lld bytes, the game's %u): a car may not load", names[i].c_str(),
                         size.QuadPart, original);
            }
        }
        const ArchiveOverlay::Piece& piece = overlay->pieces[known->second];
        const uint32_t fields[3] = {static_cast<uint32_t>(piece.offset), piece.size, piece.size};  // stored = full: raw
        std::memcpy(&head[record + 4], fields, sizeof(fields));
    }
    if (overlay->pieces.empty()) return nullptr;
    head.resize(kDirectory + count * 16);  // only the directory changes
    overlay->head = std::move(head);
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
    if (offset < overlay.base_size) {
        const size_t part = static_cast<size_t>(std::min<uint64_t>(size, overlay.base_size - offset));
        done = base(offset, out, part);
        if (offset < overlay.head.size()) {
            const size_t patched = static_cast<size_t>(std::min<uint64_t>(done, overlay.head.size() - offset));
            std::memcpy(out, overlay.head.data() + offset, patched);
        }
        if (done < part) return done;
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
            part =static_cast<size_t>(std::min<uint64_t>(size - done, p.offset + p.size - at));
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

}  // namespace xport::pc
