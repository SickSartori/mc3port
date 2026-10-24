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

// Read-only access to the game partition (XDVDFS) of an Xbox disc image: an XISO or a full Redump dump.
// The game is served straight from the image, so users never have to extract 4.6 GB of files.
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
    XdvdfsImage() = default;
    bool read_raw(uint64_t offset, void* destination, size_t size) const;

    std::FILE* file_ = nullptr;
    uint64_t partition_offset_ = 0;
    Entry root_;
    mutable std::mutex mutex_;  // the game reads from several threads
};

}  // namespace mc3
