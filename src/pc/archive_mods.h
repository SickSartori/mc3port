#pragma once

// Files of the mods folder read instead of the entries of the game's archives (assets_x.dat, assets_x.dat_1 to _5,
// "Dave" archives). The game reads an archive's directory and then each entry at the offset the directory gives; the
// archive it is handed is the original with the replaced entries' directory records pointing past its end, where the
// mod files follow, stored uncompressed. Names and the order of the entries stay the same: a mod replaces an entry,
// it cannot add one.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace xport::pc {

// Reads bytes of the archive as it is without mods; returns how many, short at its end.
using ArchiveRead = std::function<size_t(uint64_t offset, void* destination, size_t size)>;

struct ArchiveOverlay {
    struct Piece {
        uint64_t offset = 0;  // where the file starts in the archive the game sees
        uint32_t size = 0;
        HANDLE file = INVALID_HANDLE_VALUE;
    };
    uint64_t base_size = 0;
    uint64_t size = 0;           // the archive's size with the mod files after its end
    std::vector<uint8_t> head;   // the archive's first bytes (header and directory) with the records rewritten
    std::vector<Piece> pieces;   // in offset order
    std::vector<std::string> replaced;  // the entries' names, one per piece, for the log
    std::unique_ptr<std::atomic<bool>[]> read_once;  // per piece: the game has read it (logged the first time)
    ~ArchiveOverlay();
};

// The names of a Dave archive's entries in order, as the game decodes them (a name ends at the alphabet's 48th
// symbol, which the game's table makes a 0). Empty when `head` is not a whole Dave header and directory.
std::vector<std::string> dave_entry_names(const std::vector<uint8_t>& head);

// The mods folder's files by their path inside it: lower case, '/' between the parts.
std::map<std::string, std::filesystem::path> mod_files(const std::filesystem::path& folder);

// The overlay of an archive for the files of `mods` named like its entries; nullptr when it is not a Dave archive or
// no file applies. Every entry with a mod's name is replaced (a name can be in an archive more than once).
std::shared_ptr<const ArchiveOverlay> make_archive_overlay(const ArchiveRead& base, uint64_t base_size,
                                                           const std::map<std::string, std::filesystem::path>& mods);

// Reads the archive as the game sees it: the rewritten directory, the original data, the mod files after its end.
size_t read_archive_overlay(const ArchiveOverlay& overlay, const ArchiveRead& base, uint64_t offset, void* destination,
                            size_t size);

}  // namespace xport::pc
