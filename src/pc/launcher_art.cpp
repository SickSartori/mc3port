#include "pc/launcher_art.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <random>
#include <string>

#include "pc/log.h"

namespace mc3::pc {
namespace {

// An .xbck file is the image of the Flash movie as loaded at the address in its first dword; its textures are
// D3D resources (common, data, lock, format, size) whose data addresses point into the same image, 0x80 bytes in.
struct XbckTexture {
    uint32_t width, height, offset;
};

constexpr uint32_t kTextureCommon = 0x00040001;  // one reference, D3DCOMMON_TYPE_TEXTURE
constexpr uint32_t kFormatDxt5 = 0x0F;
constexpr uint32_t kHeaderBytes = 0x80;

uint32_t dword(const std::vector<uint8_t>& file, size_t offset) {
    uint32_t value;
    std::memcpy(&value, file.data() + offset, 4);
    return value;
}

std::vector<XbckTexture> dxt5_textures(const std::vector<uint8_t>& file) {
    std::vector<XbckTexture> found;
    if (file.size() < kHeaderBytes) return found;
    const uint32_t load = dword(file, 0) & 0x7FFFFFFF;
    for (size_t at = 0; at + 20 <= file.size(); at += 4) {
        const uint32_t format = dword(file, at + 12);
        if (dword(file, at) != kTextureCommon || ((format >> 4) & 0xF) != 2 || ((format >> 8) & 0xFF) != kFormatDxt5 ||
            dword(file, at + 16) != 0) {
            continue;
        }
        const uint32_t width = 1u << ((format >> 20) & 0xF), height = 1u << ((format >> 24) & 0xF);
        const uint64_t offset = static_cast<uint64_t>(dword(file, at + 4)) - load + kHeaderBytes;
        if (width < 4 || height < 4 || offset + width * height > file.size()) continue;  // DXT5: one byte per pixel
        found.push_back({width, height, static_cast<uint32_t>(offset)});
    }
    return found;
}

DxtImage copy(const std::vector<uint8_t>& file, const XbckTexture& texture) {
    DxtImage image{texture.width, texture.height, {}};
    image.blocks.assign(file.begin() + texture.offset, file.begin() + texture.offset + texture.width * texture.height);
    return image;
}

// Two 256x256 halves side by side, as the loading screen shows them: block rows interleaved.
DxtImage join(const DxtImage& left, const DxtImage& right) {
    DxtImage image{left.width + right.width, left.height, {}};
    const uint32_t left_row = left.width * 4, right_row = right.width * 4;  // bytes per row of 4x4 blocks
    for (uint32_t row = 0; row < left.height / 4; ++row) {
        image.blocks.insert(image.blocks.end(), left.blocks.begin() + row * left_row,
                            left.blocks.begin() + (row + 1) * left_row);
        image.blocks.insert(image.blocks.end(), right.blocks.begin() + row * right_row,
                            right.blocks.begin() + (row + 1) * right_row);
    }
    return image;
}

}  // namespace

LauncherArt load_launcher_art(const XdvdfsImage& disc) {
    LauncherArt art;
    if (const std::optional<XdvdfsImage::Entry> menu = disc.find("flash/bg_main.xbck")) {
        const std::vector<uint8_t> file = disc.read_all(*menu);
        for (const XbckTexture& texture : dxt5_textures(file)) {
            if (texture.width == 512 && texture.height == 128) art.logo = copy(file, texture);
        }
    }
    const std::optional<XdvdfsImage::Entry> flash = disc.find("flash");
    if (flash) {
        for (const XdvdfsImage::Entry& entry : disc.list(*flash)) {
            std::string name = entry.name;
            std::transform(name.begin(), name.end(), name.begin(), [](char c) { return static_cast<char>(std::tolower(c)); });
            if (name.rfind("loading_network_", 0) != 0 && name.rfind("loading_generic_", 0) != 0) continue;
            const std::vector<uint8_t> file = disc.read_all(entry);
            const std::vector<XbckTexture> textures = dxt5_textures(file);
            if (textures.size() < 2 || textures[0].width != 256 || textures[0].height != 256 ||
                textures[1].width != 256 || textures[1].height != 256) {
                continue;
            }
            DxtImage picture = join(copy(file, textures[0]), copy(file, textures[1]));
            const bool repeated = std::any_of(art.artwork.begin(), art.artwork.end(),
                                              [&](const DxtImage& other) { return other.blocks == picture.blocks; });
            if (!repeated) art.artwork.push_back(std::move(picture));
        }
    }
    std::shuffle(art.artwork.begin(), art.artwork.end(), std::mt19937(std::random_device{}()));
    log_line("launcher: logo %s, %zu artwork pictures from the disc", art.logo.blocks.empty() ? "not found" : "found",
             art.artwork.size());
    return art;
}

}  // namespace mc3::pc
