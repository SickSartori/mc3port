#include "core/xbe.h"

#include <cstring>

#include "core/byteorder.h"

namespace mc3 {
namespace {

// Image header offsets.
constexpr uint32_t kMagicOffset = 0x000;
constexpr uint32_t kBaseAddressOffset = 0x104;
constexpr uint32_t kHeaderSizeOffset = 0x108;
constexpr uint32_t kImageSizeOffset = 0x10C;
constexpr uint32_t kCertificateOffset = 0x118;
constexpr uint32_t kSectionCountOffset = 0x11C;
constexpr uint32_t kSectionHeadersOffset = 0x120;
constexpr uint32_t kInitFlagsOffset = 0x124;
constexpr uint32_t kEntryPointOffset = 0x128;
constexpr uint32_t kTlsAddressOffset = 0x12C;
constexpr uint32_t kStackCommitOffset = 0x130;
constexpr uint32_t kKernelThunkOffset = 0x158;
constexpr uint32_t kLibraryCountOffset = 0x160;
constexpr uint32_t kLibrariesOffset = 0x164;
constexpr uint32_t kMinimumHeaderSize = 0x178;

constexpr uint32_t kSectionHeaderSize = 56;
constexpr uint32_t kLibraryEntrySize = 16;
constexpr uint32_t kTlsDirectorySize = 24;
constexpr uint32_t kCertificateTitleIdOffset = 0x08;
constexpr uint32_t kCertificateTitleNameOffset = 0x0C;
constexpr uint32_t kTitleNameChars = 40;

// The entry point and kernel thunk are XOR-obfuscated with a key that differs between retail and debug builds.
constexpr uint32_t kEntryKeyRetail = 0xA8FC57AB;
constexpr uint32_t kEntryKeyDebug = 0x94859D4B;
constexpr uint32_t kThunkKeyRetail = 0x5B6D40B6;
constexpr uint32_t kThunkKeyDebug = 0xEFB1F152;

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

}  // namespace

std::optional<Xbe> Xbe::parse(std::vector<uint8_t> file, std::string* error) {
    Xbe xbe;
    xbe.file_ = std::move(file);
    const std::vector<uint8_t>& data = xbe.file_;
    if (data.size() < kMinimumHeaderSize || std::memcmp(data.data() + kMagicOffset, "XBEH", 4) != 0) {
        fail(error, "not an XBE file");
        return std::nullopt;
    }
    const auto u32 = [&](uint32_t offset) { return load_le32(data.data() + offset); };

    xbe.base_address_ = u32(kBaseAddressOffset);
    xbe.header_size_ = u32(kHeaderSizeOffset);
    xbe.image_size_ = u32(kImageSizeOffset);
    xbe.init_flags_ = u32(kInitFlagsOffset);
    xbe.tls_address_ = u32(kTlsAddressOffset);
    xbe.stack_commit_ = u32(kStackCommitOffset);
    if (xbe.header_size_ < kMinimumHeaderSize || xbe.header_size_ > data.size()) {
        fail(error, "XBE header size is out of range");
        return std::nullopt;
    }

    // Header-resident structures are addressed by virtual address; this turns one into a checked file offset.
    const auto header_offset = [&](uint32_t address, uint32_t size) -> std::optional<uint32_t> {
        if (address < xbe.base_address_) return std::nullopt;
        const uint32_t offset = address - xbe.base_address_;
        if (offset > xbe.header_size_ || size > xbe.header_size_ - offset) return std::nullopt;
        return offset;
    };
    const auto header_string = [&](uint32_t address) {
        std::string text;
        for (auto offset = header_offset(address, 1); offset && *offset < xbe.header_size_ && data[*offset];
             offset = *offset + 1) {
            text.push_back(static_cast<char>(data[*offset]));
        }
        return text;
    };

    const uint32_t section_count = u32(kSectionCountOffset);
    const auto sections = header_offset(u32(kSectionHeadersOffset), section_count * kSectionHeaderSize);
    if (!sections) {
        fail(error, "XBE section headers are out of range");
        return std::nullopt;
    }
    for (uint32_t i = 0; i < section_count; ++i) {
        const uint8_t* raw = data.data() + *sections + i * kSectionHeaderSize;
        XbeSection section;
        section.flags = load_le32(raw);
        section.virtual_address = load_le32(raw + 4);
        section.virtual_size = load_le32(raw + 8);
        section.raw_address = load_le32(raw + 12);
        section.raw_size = load_le32(raw + 16);
        section.name = header_string(load_le32(raw + 20));
        if (section.raw_address > data.size() || section.raw_size > data.size() - section.raw_address) {
            fail(error, "XBE section raw data is out of range");
            return std::nullopt;
        }
        xbe.sections_.push_back(std::move(section));
    }

    const uint32_t library_count = u32(kLibraryCountOffset);
    if (const auto libraries = header_offset(u32(kLibrariesOffset), library_count * kLibraryEntrySize)) {
        for (uint32_t i = 0; i < library_count; ++i) {
            const uint8_t* raw = data.data() + *libraries + i * kLibraryEntrySize;
            XbeLibrary library;
            library.name.assign(reinterpret_cast<const char*>(raw), strnlen(reinterpret_cast<const char*>(raw), 8));
            library.major = load_le16(raw + 8);
            library.minor = load_le16(raw + 10);
            library.build = load_le16(raw + 12);
            library.flags = load_le16(raw + 14);
            xbe.libraries_.push_back(std::move(library));
        }
    }

    if (const auto certificate = header_offset(u32(kCertificateOffset), kCertificateTitleNameOffset + kTitleNameChars * 2)) {
        xbe.title_id_ = u32(*certificate + kCertificateTitleIdOffset);
        for (uint32_t i = 0; i < kTitleNameChars; ++i) {
            const uint16_t ch = load_le16(data.data() + *certificate + kCertificateTitleNameOffset + i * 2);
            if (ch == 0) break;
            xbe.title_.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
        }
    }

    const uint32_t image_end = xbe.base_address_ + xbe.image_size_;
    const uint32_t entry_retail = u32(kEntryPointOffset) ^ kEntryKeyRetail;
    xbe.retail_ = entry_retail >= xbe.base_address_ && entry_retail < image_end;
    xbe.entry_point_ = xbe.retail_ ? entry_retail : u32(kEntryPointOffset) ^ kEntryKeyDebug;
    xbe.kernel_thunk_address_ = u32(kKernelThunkOffset) ^ (xbe.retail_ ? kThunkKeyRetail : kThunkKeyDebug);
    if (xbe.entry_point_ < xbe.base_address_ || xbe.entry_point_ >= image_end) {
        fail(error, "XBE entry point is outside the image");
        return std::nullopt;
    }
    return xbe;
}

std::optional<uint32_t> Xbe::file_offset(uint32_t virtual_address, uint32_t size) const {
    if (virtual_address >= base_address_ && virtual_address - base_address_ < header_size_ &&
        size <= header_size_ - (virtual_address - base_address_)) {
        return virtual_address - base_address_;
    }
    for (const XbeSection& section : sections_) {
        if (virtual_address >= section.virtual_address && virtual_address - section.virtual_address < section.raw_size &&
            size <= section.raw_size - (virtual_address - section.virtual_address)) {
            return section.raw_address + (virtual_address - section.virtual_address);
        }
    }
    return std::nullopt;
}

std::optional<XbeTlsDirectory> Xbe::tls() const {
    if (tls_address_ == 0) return std::nullopt;
    const auto offset = file_offset(tls_address_, kTlsDirectorySize);
    if (!offset) return std::nullopt;
    const uint8_t* raw = file_.data() + *offset;
    XbeTlsDirectory directory;
    directory.raw_data_start = load_le32(raw);
    directory.raw_data_end = load_le32(raw + 4);
    directory.index_address = load_le32(raw + 8);
    directory.callbacks_address = load_le32(raw + 12);
    directory.zero_fill_size = load_le32(raw + 16);
    directory.characteristics = load_le32(raw + 20);
    return directory;
}

}  // namespace mc3
