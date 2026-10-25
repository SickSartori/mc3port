#include "pc/nv2a_texture.h"

#include <algorithm>
#include <cstring>
#include <set>

#include "pc/log.h"
#include "pc/physical_memory.h"

namespace mc3::pc {
namespace {

enum Kind { kSwizzled, kLinear, kCompressed };

struct FormatInfo {
    uint8_t code;
    Kind kind;
    uint8_t bytes_per_texel;  // compressed: bytes per 4x4 block
    const char* name;
};

// NV097_SET_TEXTURE_FORMAT_COLOR values handled so far.
constexpr FormatInfo kFormats[] = {
    {0x00, kSwizzled, 1, "SZ_Y8"},           {0x01, kSwizzled, 1, "SZ_AY8"},
    {0x02, kSwizzled, 2, "SZ_A1R5G5B5"},     {0x03, kSwizzled, 2, "SZ_X1R5G5B5"},
    {0x04, kSwizzled, 2, "SZ_A4R4G4B4"},     {0x05, kSwizzled, 2, "SZ_R5G6B5"},
    {0x06, kSwizzled, 4, "SZ_A8R8G8B8"},     {0x07, kSwizzled, 4, "SZ_X8R8G8B8"},
    {0x0B, kSwizzled, 1, "SZ_I8_A8R8G8B8"},  {0x0C, kCompressed, 8, "L_DXT1"},
    {0x0E, kCompressed, 16, "L_DXT23"},      {0x0F, kCompressed, 16, "L_DXT45"},
    {0x10, kLinear, 2, "LU_A1R5G5B5"},       {0x11, kLinear, 2, "LU_R5G6B5"},
    {0x12, kLinear, 4, "LU_A8R8G8B8"},       {0x13, kLinear, 1, "LU_Y8"},
    {0x19, kSwizzled, 1, "SZ_A8"},           {0x1A, kSwizzled, 2, "SZ_A8Y8"},
    {0x1B, kLinear, 1, "LU_AY8"},            {0x1C, kLinear, 2, "LU_X1R5G5B5"},
    {0x1D, kLinear, 2, "LU_A4R4G4B4"},       {0x1E, kLinear, 4, "LU_X8R8G8B8"},
    {0x1F, kLinear, 1, "LU_A8"},             {0x20, kLinear, 2, "LU_A8Y8"},
    {0x24, kLinear, 2, "LC_YUY2"},           {0x25, kLinear, 2, "LC_UYVY"},
    {0x3A, kSwizzled, 4, "SZ_A8B8G8R8"},     {0x3F, kLinear, 4, "LU_A8B8G8R8"},
};

const FormatInfo* format_info(uint32_t code) {
    for (const FormatInfo& info : kFormats) {
        if (info.code == code) return &info;
    }
    return nullptr;
}

uint32_t color_code(const TextureDescriptor& texture) { return (texture.format >> 8) & 0xFF; }

// Morton order as the NV2A swizzles it: bits of x and y alternate from the lowest, x first, while both dimensions
// still have bits left; the remaining bits belong to the larger dimension.
void swizzle_masks(uint32_t width, uint32_t height, uint32_t* mask_x, uint32_t* mask_y) {
    uint32_t x = 0, y = 0, bit = 1, mask_bit = 1;
    bool done;
    do {
        done = true;
        if (bit < width) {
            x |= mask_bit;
            mask_bit <<= 1;
            done = false;
        }
        if (bit < height) {
            y |= mask_bit;
            mask_bit <<= 1;
            done = false;
        }
        bit <<= 1;
    } while (!done);
    *mask_x = x;
    *mask_y = y;
}

uint32_t deposit(uint32_t value, uint32_t mask) {
    uint32_t result = 0;
    for (uint32_t bit = 1; mask; bit <<= 1) {
        const uint32_t lowest = mask & (~mask + 1);
        if (value & bit) result |= lowest;
        mask &= mask - 1;
    }
    return result;
}

uint8_t expand(uint32_t value, uint32_t bits) {
    return static_cast<uint8_t>(bits >= 8 ? value : (value << (8 - bits)) | (value >> (2 * bits - 8 > 0 ? 2 * bits - 8 : 0)));
}

void put(uint8_t* out, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    out[0] = b;
    out[1] = g;
    out[2] = r;
    out[3] = a;
}

uint8_t clamp_byte(int32_t value) { return static_cast<uint8_t>(std::clamp(value, 0, 255)); }

// One texel to BGRA8.
void convert_texel(uint32_t code, const uint8_t* in, const uint8_t* palette, uint8_t* out) {
    switch (code) {
        case 0x06: case 0x12: put(out, in[2], in[1], in[0], in[3]); break;
        case 0x07: case 0x1E: put(out, in[2], in[1], in[0], 255); break;
        case 0x3A: case 0x3F: put(out, in[0], in[1], in[2], in[3]); break;
        case 0x00: case 0x13: put(out, in[0], in[0], in[0], 255); break;
        case 0x01: case 0x1B: put(out, in[0], in[0], in[0], in[0]); break;
        case 0x19: case 0x1F: put(out, 0, 0, 0, in[0]); break;
        case 0x1A: case 0x20: put(out, in[0], in[0], in[0], in[1]); break;
        case 0x0B: {
            const uint8_t* entry = palette ? palette + in[0] * 4 : nullptr;
            if (entry) {
                put(out, entry[2], entry[1], entry[0], entry[3]);
            } else {
                put(out, 255, 0, 255, 255);
            }
            break;
        }
        default: {
            const uint32_t v = in[0] | (in[1] << 8);
            switch (code) {
                case 0x05: case 0x11:
                    put(out, expand(v >> 11, 5), expand((v >> 5) & 63, 6), expand(v & 31, 5), 255);
                    break;
                case 0x02: case 0x10:
                    put(out, expand((v >> 10) & 31, 5), expand((v >> 5) & 31, 5), expand(v & 31, 5), v & 0x8000 ? 255 : 0);
                    break;
                case 0x03: case 0x1C:
                    put(out, expand((v >> 10) & 31, 5), expand((v >> 5) & 31, 5), expand(v & 31, 5), 255);
                    break;
                case 0x04: case 0x1D:
                    put(out, expand((v >> 8) & 15, 4), expand((v >> 4) & 15, 4), expand(v & 15, 4), expand(v >> 12, 4));
                    break;
                default: put(out, 255, 0, 255, 255); break;
            }
            break;
        }
    }
}

// YUY2 / UYVY: two pixels per 4 bytes sharing chroma (BT.601, video range).
void convert_yuv_pair(uint32_t code, const uint8_t* in, uint8_t* out) {
    const bool yuy2 = code == 0x24;
    const int y0 = yuy2 ? in[0] : in[1];
    const int u = yuy2 ? in[1] : in[0];
    const int y1 = yuy2 ? in[2] : in[3];
    const int v = yuy2 ? in[3] : in[2];
    for (int i = 0; i < 2; ++i) {
        const int c = (i ? y1 : y0) - 16, d = u - 128, e = v - 128;
        put(out + i * 4, clamp_byte((298 * c + 409 * e + 128) >> 8), clamp_byte((298 * c - 100 * d - 208 * e + 128) >> 8),
            clamp_byte((298 * c + 516 * d + 128) >> 8), 255);
    }
}

std::set<uint32_t> g_reported_formats;

}  // namespace

bool texture_extent(const TextureDescriptor& texture, uint32_t* width, uint32_t* height, uint32_t* bytes) {
    const FormatInfo* info = format_info(color_code(texture));
    if (!info) return false;
    if (info->kind == kLinear) {
        *width = texture.rect >> 16;
        *height = texture.rect & 0xFFFF;
        *bytes = texture.pitch * *height;
    } else {
        *width = 1u << ((texture.format >> 20) & 0xF);
        *height = 1u << ((texture.format >> 24) & 0xF);
        *bytes = info->kind == kCompressed ? std::max(1u, *width / 4) * std::max(1u, *height / 4) * info->bytes_per_texel
                                           : *width * *height * info->bytes_per_texel;
    }
    return *width && *height && *width <= 4096 && *height <= 4096;
}

bool texture_is_linear(const TextureDescriptor& texture) {
    const FormatInfo* info = format_info(color_code(texture));
    return info && info->kind == kLinear;
}

TextureImage decode_texture(const TextureDescriptor& texture) {
    TextureImage image;
    const uint32_t code = color_code(texture);
    const FormatInfo* info = format_info(code);
    uint32_t width = 0, height = 0, bytes = 0;
    if (!info || !texture_extent(texture, &width, &height, &bytes)) {
        if (g_reported_formats.insert(code).second) log_line("nv2a: texture format %02X not supported yet", code);
        return image;
    }
    const uint8_t* source = physical_pointer(texture.address);
    if (!source || !physical_pointer(texture.address + bytes - 1)) return image;
    image.width = width;
    image.height = height;

    if (info->kind == kCompressed) {
        image.layout = code == 0x0C ? TextureLayout::Bc1 : code == 0x0E ? TextureLayout::Bc2 : TextureLayout::Bc3;
        image.row_pitch = std::max(1u, width / 4) * info->bytes_per_texel;
        image.data.assign(source, source + bytes);
        return image;
    }

    image.layout = TextureLayout::Bgra8;
    image.row_pitch = width * 4;
    image.data.resize(static_cast<size_t>(width) * height * 4);
    const uint8_t* palette = code == 0x0B ? physical_pointer(texture.palette) : nullptr;

    if (code == 0x24 || code == 0x25) {
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x + 1 < width; x += 2) {
                convert_yuv_pair(code, source + y * texture.pitch + x * 2, image.data.data() + (y * width + x) * 4);
            }
        }
        return image;
    }

    if (info->kind == kLinear) {
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                convert_texel(code, source + y * texture.pitch + x * info->bytes_per_texel, palette,
                              image.data.data() + (y * width + x) * 4);
            }
        }
        return image;
    }

    uint32_t mask_x = 0, mask_y = 0;
    swizzle_masks(width, height, &mask_x, &mask_y);
    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t row = deposit(y, mask_y);
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t index = row | deposit(x, mask_x);
            convert_texel(code, source + index * info->bytes_per_texel, palette, image.data.data() + (y * width + x) * 4);
        }
    }
    return image;
}

}  // namespace mc3::pc
