#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mc3 {

// Read-only access to the game partition (XDVDFS) of an Xbox disc image: an XISO or a full Redump dump, in one file
// or split in parts (name.1.iso, name.2.iso: 4.5 GB is too much for the Xbox's own file system), plain or compressed
// as CCI (name.cci, name.1.cci...). The game is served straight from the image, so users never have to extract it.
class XdvdfsImage {
public:
    struct Entry {
        std::string name;
        uint32_t sector = 0;
        uint32_t size = 0;
        bool is_directory = false;
    };

    static std::unique_ptr<XdvdfsImage> open(const std::filesystem::path& path, std::string* error);

    XdvdfsImage(const XdvdfsImage&) = delete;
    XdvdfsImage& operator=(const XdvdfsImage&) = delete;
    ~XdvdfsImage();

    const Entry& root() const { return root_; }
    std::vector<Entry> list(const Entry& directory) const;

    // Components separated by '\' or '/', matched case-insensitively like the Xbox does.
    // An empty path returns the root directory.
    std::optional<Entry> find(std::string_view path) const;

    // Returns the number of bytes read, short at end of file and 0 on error.
    size_t read(const Entry& file, uint64_t offset, void* destination, size_t size) const;
    std::vector<uint8_t> read_all(const Entry& file) const;

private:
    // One file of the image: its bytes from `start` of the whole image on, `size` of them. A CCI part keeps the
    // position of each 2048-byte sector (`sectors`, one more for the end), compressed with LZ4 where flagged.
    struct Part {
        std::FILE* file = nullptr;
        uint64_t start = 0;
        uint64_t size = 0;
        std::vector<uint32_t> sectors;
    };

    XdvdfsImage() = default;
    bool read_raw(uint64_t offset, void* destination, size_t size) const;
    bool read_cci_sector(const Part& part, uint64_t sector, uint8_t* destination) const;

    std::vector<Part> parts_;
    uint64_t partition_offset_ = 0;
    Entry root_;
    mutable std::mutex mutex_;  // the game reads from several threads
};

}  // namespace mc3
