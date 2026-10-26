#pragma once

// NV2A textures to host images. Swizzled ("SZ_") formats store texels in Morton order; linear ("LU_"/"LC_")
// formats have a pitch; DXT formats are block-compressed and stay compressed.

#include <cstdint>
#include <vector>

namespace mc3::pc {

struct TextureDescriptor {
    uint32_t address;   // physical
    uint32_t format;    // NV097_SET_TEXTURE_FORMAT
    uint32_t pitch;     // linear formats (NV097_SET_TEXTURE_CONTROL1 >> 16)
    uint32_t rect;      // linear formats (NV097_SET_TEXTURE_IMAGE_RECT: width << 16 | height)
    uint32_t palette;   // physical address of the palette (I8 formats)
};

enum class TextureLayout { Bgra8, Bc1, Bc2, Bc3, Unsupported };

struct TextureImage {
    uint32_t width = 0;
    uint32_t height = 0;
    TextureLayout layout = TextureLayout::Unsupported;
    uint32_t row_pitch = 0;  // bytes per row (BC: per row of 4x4 blocks)
    std::vector<uint8_t> data;
};

// Size in texels of the top level, and the number of bytes of guest memory the top level occupies.
bool texture_extent(const TextureDescriptor& texture, uint32_t* width, uint32_t* height, uint32_t* bytes);

// Linear formats are rectangle textures: the hardware addresses them in texels, not in 0..1.
bool texture_is_linear(const TextureDescriptor& texture);

// Mip levels stored after the top one (NV097_SET_TEXTURE_FORMAT bits 16-19; 1 for linear textures), the
// descriptor of one level (its address and size, one level), and the bytes all the levels occupy.
uint32_t texture_levels(const TextureDescriptor& texture);
TextureDescriptor texture_level(const TextureDescriptor& texture, uint32_t level);
uint32_t texture_total_bytes(const TextureDescriptor& texture);

// Decodes the top mip level. Unsupported formats return layout Unsupported (logged once per format).
TextureImage decode_texture(const TextureDescriptor& texture);

}  // namespace mc3::pc
