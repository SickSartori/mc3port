// NV2A textures to images, and the enlarger of the HUD's pictures and the cars' stickers. The guest's memory is a buffer.

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "pc/log.h"
#include "pc/nv2a_texture.h"
#include "pc/physical_memory.h"
#include "pc/texture_upscale.h"
#include "test.h"

namespace {
std::vector<uint8_t> g_memory(1024 * 1024);
}

// nv2a_texture.cpp's only ties to the rest of the program.
namespace mc3::pc {
void log_line(const char*, ...) {}
uint8_t* physical_pointer(uint32_t address) { return address < g_memory.size() ? g_memory.data() + address : nullptr; }
}  // namespace mc3::pc

using namespace mc3::pc;

namespace {

constexpr uint32_t kTexture = 0x1000, kPalette = 0x800;

uint32_t format(uint32_t code, uint32_t log_u, uint32_t log_v, uint32_t levels = 1) {
    return code << 8 | levels << 16 | log_u << 20 | log_v << 24 | 0x29;
}

const uint8_t* texel(const TextureImage& image, uint32_t x, uint32_t y) {
    return image.data.data() + y * image.row_pitch + x * 4;
}

bool is(const uint8_t* bgra, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    return bgra[0] == b && bgra[1] == g && bgra[2] == r && bgra[3] == a;
}

void test_extents() {
    uint32_t w = 0, h = 0, bytes = 0;
    // The format a car's sticker came with.
    const TextureDescriptor livery{kTexture, 0x08810B29, 0, 0, kPalette};
    CHECK(texture_extent(livery, &w, &h, &bytes) && w == 256 && h == 256 && bytes == 65536);
    CHECK(!texture_is_linear(livery));
    CHECK(texture_levels(livery) == 1);
    CHECK(texture_total_bytes(livery) == 65536);

    // Mip levels follow the top one: DXT1, 16 x 8, 3 levels: 64 + 16 + 8 bytes (a level is at least one block).
    const TextureDescriptor dxt{kTexture, format(0x0C, 4, 3, 3)};
    CHECK(texture_extent(dxt, &w, &h, &bytes) && w == 16 && h == 8 && bytes == 64);
    CHECK(texture_levels(dxt) == 3);
    const TextureDescriptor second = texture_level(dxt, 1), third = texture_level(dxt, 2);
    CHECK(second.address == kTexture + 64 && third.address == kTexture + 64 + 16);
    CHECK(texture_extent(third, &w, &h, &bytes) && w == 4 && h == 2 && bytes == 8);
    CHECK(texture_total_bytes(dxt) == 88);

    // More levels than the size allows, and cube maps: clamped.
    CHECK(texture_levels({kTexture, format(0x06, 1, 1, 9)}) == 2);
    CHECK(texture_levels({kTexture, format(0x06, 3, 3, 4) | 0x4}) == 1);

    const TextureDescriptor linear{kTexture, format(0x12, 0, 0), 40, 10 << 16 | 3};
    CHECK(texture_is_linear(linear) && texture_levels(linear) == 1);
    CHECK(texture_extent(linear, &w, &h, &bytes) && w == 10 && h == 3 && bytes == 120);

    CHECK(!texture_extent({kTexture, format(0x7E, 2, 2)}, &w, &h, &bytes));
}

void test_swizzle() {
    // A8R8G8B8, 4 x 4: the blue byte is the index in memory, in Morton order, x's bit first.
    for (uint32_t i = 0; i < 16; ++i) g_memory[kTexture + i * 4] = static_cast<uint8_t>(i);
    TextureImage image = decode_texture({kTexture, format(0x06, 2, 2)});
    CHECK(image.layout == TextureLayout::Bgra8 && image.width == 4 && image.height == 4);
    constexpr uint8_t kSquare[4][4] = {{0, 1, 4, 5}, {2, 3, 6, 7}, {8, 9, 12, 13}, {10, 11, 14, 15}};
    for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 4; ++x) CHECK(texel(image, x, y)[0] == kSquare[y][x]);
    }
    // 4 x 2: y runs out of bits first, the rest belong to x.
    image = decode_texture({kTexture, format(0x06, 2, 1)});
    constexpr uint8_t kWide[2][4] = {{0, 1, 4, 5}, {2, 3, 6, 7}};
    for (uint32_t y = 0; y < 2; ++y) {
        for (uint32_t x = 0; x < 4; ++x) CHECK(texel(image, x, y)[0] == kWide[y][x]);
    }
    // 2 x 4: x runs out first.
    image = decode_texture({kTexture, format(0x06, 1, 2)});
    constexpr uint8_t kTall[4][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}};
    for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 2; ++x) CHECK(texel(image, x, y)[0] == kTall[y][x]);
    }
}

void test_texels() {
    uint8_t* in = g_memory.data() + kTexture;
    const auto one = [&](uint32_t code) { return decode_texture({kTexture, format(code, 0, 0)}); };

    in[0] = 0x10, in[1] = 0x20, in[2] = 0x30, in[3] = 0x40;
    CHECK(is(texel(one(0x06), 0, 0), 0x10, 0x20, 0x30, 0x40));  // A8R8G8B8
    CHECK(is(texel(one(0x07), 0, 0), 0x10, 0x20, 0x30, 255));   // X8R8G8B8
    CHECK(is(texel(one(0x3A), 0, 0), 0x30, 0x20, 0x10, 0x40));  // A8B8G8R8
    CHECK(is(texel(one(0x00), 0, 0), 0x10, 0x10, 0x10, 255));   // Y8
    CHECK(is(texel(one(0x19), 0, 0), 0, 0, 0, 0x10));           // A8
    CHECK(is(texel(one(0x1A), 0, 0), 0x10, 0x10, 0x10, 0x20));  // A8Y8

    in[0] = 0xFF, in[1] = 0xFF;
    CHECK(is(texel(one(0x05), 0, 0), 255, 255, 255, 255));  // R5G6B5: full is 255, not 248
    in[0] = 0x00, in[1] = 0xF8;
    CHECK(is(texel(one(0x05), 0, 0), 0, 0, 255, 255));
    in[0] = 0x00, in[1] = 0x80;
    CHECK(is(texel(one(0x02), 0, 0), 0, 0, 0, 255));  // A1R5G5B5
    in[1] = 0x00;
    CHECK(is(texel(one(0x02), 0, 0), 0, 0, 0, 0));
    in[0] = 0x21, in[1] = 0xF3;
    CHECK(is(texel(one(0x04), 0, 0), 0x11, 0x22, 0x33, 0xFF));  // A4R4G4B4

    // A palette texture: entries are B, G, R, A.
    in[0] = 2;
    const uint8_t entry[4] = {0x0A, 0x0B, 0x0C, 0x0D};
    std::memcpy(g_memory.data() + kPalette + 2 * 4, entry, 4);
    CHECK(is(texel(decode_texture({kTexture, format(0x0B, 0, 0), 0, 0, kPalette}), 0, 0), 0x0A, 0x0B, 0x0C, 0x0D));

    // Linear: by rows of `pitch`.
    std::memset(in, 0, 64);
    in[24] = 0x55;  // row 1 (pitch 16), texel 2
    const TextureImage linear = decode_texture({kTexture, format(0x12, 0, 0), 16, 3 << 16 | 2});
    CHECK(linear.width == 3 && linear.height == 2 && texel(linear, 2, 1)[0] == 0x55 && texel(linear, 1, 1)[0] == 0);

    // YUY2: white and black, video range.
    in[0] = 235, in[1] = 128, in[2] = 16, in[3] = 128;
    const TextureImage video = decode_texture({kTexture, format(0x24, 0, 0), 4, 2 << 16 | 1});
    CHECK(is(texel(video, 0, 0), 255, 255, 255, 255) && is(texel(video, 1, 0), 0, 0, 0, 255));

    // Compressed ones stay so, with the pitch of a row of blocks.
    const TextureImage dxt = decode_texture({kTexture, format(0x0F, 3, 2)});
    CHECK(dxt.layout == TextureLayout::Bc3 && dxt.row_pitch == 32 && dxt.data.size() == 32);

    // Unknown format, or memory past the end: nothing, not a crash.
    CHECK(decode_texture({kTexture, format(0x7E, 2, 2)}).layout == TextureLayout::Unsupported);
    CHECK(decode_texture({static_cast<uint32_t>(g_memory.size()) - 4, format(0x06, 4, 4)}).layout == TextureLayout::Unsupported);
}

TextureImage bgra(uint32_t width, uint32_t height) {
    TextureImage image;
    image.width = width, image.height = height, image.row_pitch = width * 4;
    image.layout = TextureLayout::Bgra8;
    image.data.resize(size_t{width} * height * 4);
    return image;
}

void test_enlarger() {
    // Sizes: four times up to 128, twice above, not at all past 512; mips down to 1 x 1, each half the last.
    CHECK(upscale_2d_texture(bgra(64, 64))[0].width == 256);
    CHECK(upscale_2d_texture(bgra(128, 64))[0].width == 512);
    CHECK(upscale_2d_texture(bgra(256, 256))[0].width == 512);
    CHECK(upscale_2d_texture(bgra(1024, 1024)).empty());
    CHECK(upscale_2d_texture(bgra(2, 2)).empty());
    const auto levels = upscale_2d_texture(bgra(256, 128));
    CHECK(levels.size() == 10);
    for (size_t i = 1; i < levels.size(); ++i) {
        CHECK(levels[i].width == std::max(1u, levels[i - 1].width / 2));
        CHECK(levels[i].height == std::max(1u, levels[i - 1].height / 2));
        CHECK(levels[i].data.size() == size_t{levels[i].width} * levels[i].height * 4);
    }
    CHECK(levels.back().width == 1 && levels.back().height == 1);

    // One colour stays that colour exactly.
    TextureImage solid = bgra(32, 32);
    for (size_t i = 0; i < solid.data.size(); i += 4) solid.data[i] = 10, solid.data[i + 1] = 20, solid.data[i + 2] = 30, solid.data[i + 3] = 255;
    for (const TextureImage& level : upscale_2d_texture(solid)) {
        for (size_t i = 0; i < level.data.size(); i += 4) CHECK(is(level.data.data() + i, 10, 20, 30, 255));
    }

    // A sticker's diagonal stripe: no overshoot around the edge, and the steps of the small picture smoothed. White on
    // black: along the stripe, the enlarged edge must not stay a stair of two-texel treads.
    TextureImage stripe = bgra(64, 64);
    for (uint32_t y = 0; y < 64; ++y) {
        for (uint32_t x = 0; x < 64; ++x) {
            const uint8_t v = x >= y && x < y + 8 ? 255 : 0;
            uint8_t* p = stripe.data.data() + (size_t{y} * 64 + x) * 4;
            p[0] = p[1] = p[2] = v, p[3] = 255;
        }
    }
    const TextureImage large = upscale_2d_texture(stripe)[0];
    CHECK(large.width == 256);
    if (large.width != 256) return;
    int plain_edges = 0, large_edges = 0;  // in the middle, away from the picture's sides
    for (uint32_t y = 40; y < 216; ++y) {
        // Where the stripe's left edge meets this row, it must move right as the row goes down, never back.
        uint32_t edge = 0;
        while (edge < 255 && texel(large, edge, y)[0] < 128) ++edge;
        uint32_t next = 0;
        while (next < 255 && texel(large, next, y + 1)[0] < 128) ++next;
        CHECK(next >= edge && next <= edge + 4);
        large_edges += next != edge;
        plain_edges += (y % 4 == 0);
    }
    CHECK(large_edges >= plain_edges);  // the edge moves on more rows than the plain 4x stair would

    // Half see-through: the colour of the opaque part does not fade into a dark fringe.
    TextureImage half = bgra(32, 32);
    for (uint32_t y = 0; y < 32; ++y) {
        for (uint32_t x = 0; x < 16; ++x) {
            uint8_t* p = half.data.data() + (size_t{y} * 32 + x) * 4;
            p[2] = 200, p[3] = 255;
        }
    }
    const TextureImage fringe = upscale_2d_texture(half)[0];
    for (uint32_t x = 0; x < fringe.width; ++x) {
        const uint8_t* p = texel(fringe, x, 60);
        if (p[3] > 8) CHECK(p[2] >= 190 && p[2] <= 210);
    }

    // DXT1: one solid block repeated, decoded, enlarged; colour 0 is R5G6B5 full red.
    TextureImage block;
    block.width = block.height = 8, block.row_pitch = 16, block.layout = TextureLayout::Bc1;
    block.data.assign(32, 0);
    for (size_t i = 0; i < block.data.size(); i += 8) block.data[i + 1] = 0xF8;  // colour 0; colour 1 black; indices 0
    const auto decoded = upscale_2d_texture(block);
    CHECK(!decoded.empty() && decoded[0].width == 32 && is(texel(decoded[0], 16, 16), 0, 0, 255, 255));
}

}  // namespace

int main() {
    test_extents();
    test_swizzle();
    test_texels();
    test_enlarger();
    return test_result();
}
