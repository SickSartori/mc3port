#include "core/xdvdfs.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "core/byteorder.h"

namespace mc3 {
namespace {

constexpr uint64_t kSectorSize = 2048;
constexpr uint64_t kVolumeDescriptorSector = 32;
constexpr char kVolumeMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr size_t kVolumeMagicLength = sizeof(kVolumeMagic) - 1;

// Start of the game partition: 0 for an XISO, otherwise the XGD layouts of full disc dumps.
constexpr uint64_t kPartitionOffsets[] = {0, 0x18300000, 0xFD90000, 0x2080000};

// Directory entry: u16 left, u16 right (in 4-byte units), u32 sector, u32 size, u8 attributes, u8 name length, name.
constexpr size_t kEntryHeaderSize = 14;
constexpr uint16_t kPaddingMarker = 0xFFFF;
constexpr uint8_t kAttributeDirectory = 0x10;

std::FILE* open_file(const std::filesystem::path& path) {
#if defined(_WIN32)
    return _wfopen(path.c_str(), L"rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

bool seek_file(std::FILE* file, uint64_t offset) {
#if defined(_WIN32)
    return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

bool equals_ignore_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

}  // namespace

std::unique_ptr<XdvdfsImage> XdvdfsImage::open(const std::filesystem::path& path, std::string* error) {
    std::unique_ptr<XdvdfsImage> image(new XdvdfsImage());
    image->file_ = open_file(path);
    if (!image->file_) {
        if (error) *error = "cannot open " + path_to_utf8(path);
        return nullptr;
    }
    for (uint64_t partition : kPartitionOffsets) {
        uint8_t descriptor[kSectorSize];
        if (!image->read_raw(partition + kVolumeDescriptorSector * kSectorSize, descriptor, sizeof(descriptor))) {
            continue;
        }
        if (std::memcmp(descriptor, kVolumeMagic, kVolumeMagicLength) != 0) continue;
        image->partition_offset_ = partition;
        image->root_.sector = load_le32(descriptor + 20);
        image->root_.size = load_le32(descriptor + 24);
        image->root_.is_directory = true;
        return image;
    }
    if (error) *error = "no Xbox game partition (XDVDFS) found in " + path_to_utf8(path);
    return nullptr;
}

XdvdfsImage::~XdvdfsImage() {
    if (file_) std::fclose(file_);
}

std::vector<XdvdfsImage::Entry> XdvdfsImage::list(const Entry& directory) const {
    std::vector<Entry> entries;
    if (!directory.is_directory || directory.size == 0) return entries;
    std::vector<uint8_t> table(directory.size);
    if (!read_raw(partition_offset_ + directory.sector * kSectorSize, table.data(), table.size())) return entries;

    // Entries form a binary tree rooted at offset 0; unused space at the end of each sector is 0xFF padding.
    std::vector<uint32_t> pending{0};
    std::vector<bool> visited(table.size() / 4 + 1, false);
    while (!pending.empty()) {
        const uint32_t offset = pending.back();
        pending.pop_back();
        if (offset + kEntryHeaderSize > table.size() || visited[offset / 4]) continue;
        visited[offset / 4] = true;

        const uint8_t* raw = table.data() + offset;
        const uint16_t left = load_le16(raw);
        const uint16_t right = load_le16(raw + 2);
        if (left == kPaddingMarker && right == kPaddingMarker) continue;
        const uint8_t name_length = raw[13];
        if (offset + kEntryHeaderSize + name_length > table.size()) continue;

        Entry entry;
        entry.sector = load_le32(raw + 4);
        entry.size = load_le32(raw + 8);
        entry.is_directory = (raw[12] & kAttributeDirectory) != 0;
        entry.name.assign(reinterpret_cast<const char*>(raw + kEntryHeaderSize), name_length);
        entries.push_back(std::move(entry));

        if (left) pending.push_back(static_cast<uint32_t>(left) * 4);
        if (right) pending.push_back(static_cast<uint32_t>(right) * 4);
    }
    return entries;
}

std::optional<XdvdfsImage::Entry> XdvdfsImage::find(std::string_view path) const {
    Entry current = root_;
    while (!path.empty()) {
        const size_t separator = path.find_first_of("\\/");
        const std::string_view component = path.substr(0, separator);
        path = separator == std::string_view::npos ? std::string_view{} : path.substr(separator + 1);
        if (component.empty()) continue;

        const std::vector<Entry> children = list(current);
        const auto match = std::find_if(children.begin(), children.end(),
                                        [&](const Entry& child) { return equals_ignore_case(child.name, component); });
        if (match == children.end()) return std::nullopt;
        current = *match;
    }
    return current;
}

size_t XdvdfsImage::read(const Entry& file, uint64_t offset, void* destination, size_t size) const {
    if (file.is_directory || offset >= file.size) return 0;
    const size_t count = static_cast<size_t>(std::min<uint64_t>(size, file.size - offset));
    const uint64_t position = partition_offset_ + file.sector * kSectorSize + offset;
    return read_raw(position, destination, count) ? count : 0;
}

std::vector<uint8_t> XdvdfsImage::read_all(const Entry& file) const {
    std::vector<uint8_t> data(file.size);
    if (read(file, 0, data.data(), data.size()) != data.size()) data.clear();
    return data;
}

bool XdvdfsImage::read_raw(uint64_t offset, void* destination, size_t size) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return seek_file(file_, offset) && std::fread(destination, 1, size, file_) == size;
}

}  // namespace mc3
