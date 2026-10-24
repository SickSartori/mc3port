#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mc3 {

struct XbeSection {
    std::string name;
    uint32_t flags = 0;
    uint32_t virtual_address = 0;
    uint32_t virtual_size = 0;
    uint32_t raw_address = 0;
    uint32_t raw_size = 0;

    bool writable() const { return (flags & 0x1) != 0; }
    bool preload() const { return (flags & 0x2) != 0; }
    bool executable() const { return (flags & 0x4) != 0; }
};

struct XbeLibrary {
    std::string name;
    uint16_t major = 0;
    uint16_t minor = 0;
    uint16_t build = 0;
    uint16_t flags = 0;
};

// Addresses are virtual addresses inside the mapped image.
struct XbeTlsDirectory {
    uint32_t raw_data_start = 0;
    uint32_t raw_data_end = 0;
    uint32_t index_address = 0;
    uint32_t callbacks_address = 0;
    uint32_t zero_fill_size = 0;
    uint32_t characteristics = 0;
};

// Parsed Xbox executable. Keeps the whole file, since sections are mapped from it.
class Xbe {
public:
    static std::optional<Xbe> parse(std::vector<uint8_t> file, std::string* error);

    const std::vector<uint8_t>& file() const { return file_; }
    uint32_t base_address() const { return base_address_; }
    uint32_t header_size() const { return header_size_; }
    uint32_t image_size() const { return image_size_; }
    uint32_t entry_point() const { return entry_point_; }
    uint32_t kernel_thunk_address() const { return kernel_thunk_address_; }
    uint32_t tls_address() const { return tls_address_; }
    uint32_t stack_commit() const { return stack_commit_; }
    uint32_t init_flags() const { return init_flags_; }
    uint32_t title_id() const { return title_id_; }
    const std::string& title() const { return title_; }
    bool is_retail() const { return retail_; }
    const std::vector<XbeSection>& sections() const { return sections_; }
    const std::vector<XbeLibrary>& libraries() const { return libraries_; }

    // File offset of a virtual address in the header or in a section's raw data.
    std::optional<uint32_t> file_offset(uint32_t virtual_address, uint32_t size = 1) const;
    std::optional<XbeTlsDirectory> tls() const;

private:
    Xbe() = default;

    std::vector<uint8_t> file_;
    uint32_t base_address_ = 0;
    uint32_t header_size_ = 0;
    uint32_t image_size_ = 0;
    uint32_t entry_point_ = 0;
    uint32_t kernel_thunk_address_ = 0;
    uint32_t tls_address_ = 0;
    uint32_t stack_commit_ = 0;
    uint32_t init_flags_ = 0;
    uint32_t title_id_ = 0;
    std::string title_;
    bool retail_ = false;
    std::vector<XbeSection> sections_;
    std::vector<XbeLibrary> libraries_;
};

}  // namespace mc3
