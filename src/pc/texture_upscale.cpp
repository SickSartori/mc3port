#include "pc/texture_upscale.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mc3::pc {
namespace {

// Colour premultiplied by its alpha, 0 to 1: interpolating it puts no fringe around transparent parts.
struct Pixel {
    float r = 0, g = 0, b = 0, a = 0;
};
Pixel operator+(Pixel x, Pixel y) { return {x.r + y.r, x.g + y.g, x.b + y.b, x.a + y.a}; }
Pixel operator-(Pixel x, Pixel y) { return {x.r - y.r, x.g - y.g, x.b - y.b, x.a - y.a}; }
Pixel operator*(Pixel x, float k) { return {x.r * k, x.g * k, x.b * k, x.a * k}; }

float difference(Pixel x, Pixel y) {
    return std::fabs(x.r - y.r) + std::fabs(x.g - y.g) + std::fabs(x.b - y.b) + std::fabs(x.a - y.a);
}

struct Image {
    int width = 0, height = 0;
    std::vector<Pixel> pixels;
    Pixel& at(int x, int y) { return pixels[static_cast<size_t>(y) * width + x]; }
    Pixel get(int x, int y) const {  // the edge repeats outside
        x = std::clamp(x, 0, width - 1), y = std::clamp(y, 0, height - 1);
        return pixels[static_cast<size_t>(y) * width + x];
    }
};

Pixel from_bgra(const uint8_t* p) {
    const float a = p[3] / 255.0f;
    return {p[2] / 255.0f * a, p[1] / 255.0f * a, p[0] / 255.0f * a, a};
}

// --- BC1 to BC3 (DXT1 to DXT5): 4x4 blocks of two 5:6:5 colours and 2-bit indices, BC2 and BC3 with alpha first.

void colour_block(const uint8_t* block, bool always_four, uint8_t out[16][4]) {
    const uint32_t c0 = block[0] | block[1] << 8, c1 = block[2] | block[3] << 8;
    const auto expand = [](uint32_t c, uint8_t* bgra) {
        const uint32_t r = c >> 11 & 31, g = c >> 5 & 63, b = c & 31;
        bgra[0] = static_cast<uint8_t>(b << 3 | b >> 2);
        bgra[1] = static_cast<uint8_t>(g << 2 | g >> 4);
        bgra[2] = static_cast<uint8_t>(r << 3 | r >> 2);
        bgra[3] = 255;
    };
    uint8_t palette[4][4];
    expand(c0, palette[0]);
    expand(c1, palette[1]);
    for (int k = 0; k < 3; ++k) {
        if (always_four || c0 > c1) {
            palette[2][k] = static_cast<uint8_t>((2 * palette[0][k] + palette[1][k] + 1) / 3);
            palette[3][k] = static_cast<uint8_t>((palette[0][k] + 2 * palette[1][k] + 1) / 3);
        } else {
            palette[2][k] = static_cast<uint8_t>((palette[0][k] + palette[1][k]) / 2);
            palette[3][k] = 0;
        }
    }
    palette[2][3] = 255;
    palette[3][3] = (always_four || c0 > c1) ? 255 : 0;
    const uint32_t indices = block[4] | block[5] << 8 | block[6] << 16 | static_cast<uint32_t>(block[7]) << 24;
    for (int i = 0; i < 16; ++i) {
        const uint8_t* c = palette[indices >> (2 * i) & 3];
        std::copy(c, c + 4, out[i]);
    }
}

Image decode_bc(const TextureImage& image) {
    Image out;
    out.width = static_cast<int>(image.width), out.height = static_cast<int>(image.height);
    out.pixels.resize(static_cast<size_t>(out.width) * out.height);
    const int block_bytes = image.layout == TextureLayout::Bc1 ? 8 : 16;
    for (int by = 0; by < (out.height + 3) / 4; ++by) {
        for (int bx = 0; bx < (out.width + 3) / 4; ++bx) {
            const uint8_t* block = image.data.data() + static_cast<size_t>(by) * image.row_pitch + bx * block_bytes;
            uint8_t texels[16][4];
            if (image.layout == TextureLayout::Bc1) {
                colour_block(block, false, texels);
            } else {
                colour_block(block + 8, true, texels);
                if (image.layout == TextureLayout::Bc2) {
                    for (int i = 0; i < 16; ++i) texels[i][3] = static_cast<uint8_t>((block[i / 2] >> (4 * (i & 1)) & 15) * 17);
                } else {
                    const uint32_t a0 = block[0], a1 = block[1];
                    uint32_t alphas[8] = {a0, a1};
                    for (uint32_t k = 2; k < 8; ++k) {
                        alphas[k] = a0 > a1 ? ((8 - k) * a0 + (k - 1) * a1) / 7
                                    : k < 6 ? ((6 - k) * a0 + (k - 1) * a1) / 5
                                    : k == 6 ? 0u
                                             : 255u;
                    }
                    uint64_t bits = 0;
                    for (int k = 0; k < 6; ++k) bits |= static_cast<uint64_t>(block[2 + k]) << (8 * k);
                    for (int i = 0; i < 16; ++i) texels[i][3] = static_cast<uint8_t>(alphas[bits >> (3 * i) & 7]);
                }
            }
            for (int i = 0; i < 16; ++i) {
                const int x = bx * 4 + i % 4, y = by * 4 + i / 4;
                if (x < out.width && y < out.height) out.at(x, y) = from_bgra(texels[i]);
            }
        }
    }
    return out;
}

// The compression's 4x4 blocks show as steps across their borders: small steps there are softened, the real
// edges (a large step) stay.
void soften_block_borders(Image& image) {
    constexpr float kStep = 0.08f;
    const auto soften = [&](Pixel& p, Pixel& q) {
        const Pixel d = q - p;
        if (std::max({std::fabs(d.r), std::fabs(d.g), std::fabs(d.b), std::fabs(d.a)}) >= kStep) return;
        p = p + d * 0.25f;
        q = q - d * 0.25f;
    };
    for (int y = 0; y < image.height; ++y) {
        for (int x = 3; x + 1 < image.width; x += 4) soften(image.at(x, y), image.at(x + 1, y));
    }
    for (int y = 3; y + 1 < image.height; y += 4) {
        for (int x = 0; x < image.width; ++x) soften(image.at(x, y), image.at(x, y + 1));
    }
}

Pixel cubic(Pixel a, Pixel b, Pixel c, Pixel d) { return (b + c) * (9.0f / 16) - (a + d) * (1.0f / 16); }

// Between two interpolations across the two directions, the one along the edge (where the picture changes less):
// one when the difference is clear, else a blend that leans on it. Then kept within the neighbours, so that no
// ring of overshoot forms around an edge (directional cubic convolution, Zhou, Shen and Huang, 2012).
Pixel choose(Pixel along_a, float change_along_a, Pixel along_b, float change_along_b, Pixel n0, Pixel n1, Pixel n2,
             Pixel n3) {
    Pixel v;
    if (1 + change_along_b > 1.15f * (1 + change_along_a)) {
        v = along_a;
    } else if (1 + change_along_a > 1.15f * (1 + change_along_b)) {
        v = along_b;
    } else {
        const float wa = 1 / (1 + std::pow(change_along_a, 5.0f)), wb = 1 / (1 + std::pow(change_along_b, 5.0f));
        v = (along_a * wa + along_b * wb) * (1 / (wa + wb));
    }
    const auto keep = [](float value, float a, float b, float c, float d) {
        return std::clamp(value, std::min({a, b, c, d}), std::max({a, b, c, d}));
    };
    return {keep(v.r, n0.r, n1.r, n2.r, n3.r), keep(v.g, n0.g, n1.g, n2.g, n3.g), keep(v.b, n0.b, n1.b, n2.b, n3.b),
            keep(v.a, n0.a, n1.a, n2.a, n3.a)};
}

Image twice(const Image& in) {
    const int w = in.width, h = in.height;
    Image out;
    out.width = w * 2, out.height = h * 2;
    out.pixels.resize(static_cast<size_t>(out.width) * out.height);
    const auto P = [&](int x, int y) { return in.get(x, y); };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) out.at(2 * x, 2 * y) = P(x, y);
    }
    // The centres between four texels: along one diagonal or the other.
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float back = 0, forward = 0;  // change along "\" and along "/"
            for (int j = y - 1; j <= y + 1; ++j) {
                for (int i = x - 1; i <= x + 1; ++i) {
                    back += difference(P(i, j), P(i + 1, j + 1));
                    forward += difference(P(i + 1, j), P(i, j + 1));
                }
            }
            out.at(2 * x + 1, 2 * y + 1) =
                choose(cubic(P(x - 1, y - 1), P(x, y), P(x + 1, y + 1), P(x + 2, y + 2)), back,
                       cubic(P(x + 2, y - 1), P(x + 1, y), P(x, y + 1), P(x - 1, y + 2)), forward, P(x, y),
                       P(x + 1, y), P(x, y + 1), P(x + 1, y + 1));
        }
    }
    const auto D = [&](int x, int y) {  // the centre to the lower right of texel (x, y)
        x = std::clamp(x, 0, w - 1), y = std::clamp(y, 0, h - 1);
        return out.at(2 * x + 1, 2 * y + 1);
    };
    // The points between two texels across, and between two down: across or down, with the centres beside them.
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float across = difference(P(x - 1, y), P(x, y)) + difference(P(x, y), P(x + 1, y)) +
                           difference(P(x + 1, y), P(x + 2, y)) + difference(D(x - 1, y - 1), D(x, y - 1)) +
                           difference(D(x, y - 1), D(x + 1, y - 1)) + difference(D(x - 1, y), D(x, y)) +
                           difference(D(x, y), D(x + 1, y));
            float down = difference(D(x, y - 2), D(x, y - 1)) + difference(D(x, y - 1), D(x, y)) +
                         difference(D(x, y), D(x, y + 1)) + difference(P(x, y - 1), P(x, y)) +
                         difference(P(x, y), P(x, y + 1)) + difference(P(x + 1, y - 1), P(x + 1, y)) +
                         difference(P(x + 1, y), P(x + 1, y + 1));
            out.at(2 * x + 1, 2 * y) = choose(cubic(P(x - 1, y), P(x, y), P(x + 1, y), P(x + 2, y)), across,
                                              cubic(D(x, y - 2), D(x, y - 1), D(x, y), D(x, y + 1)), down, P(x, y),
                                              P(x + 1, y), D(x, y - 1), D(x, y));
            down = difference(P(x, y - 1), P(x, y)) + difference(P(x, y), P(x, y + 1)) +
                   difference(P(x, y + 1), P(x, y + 2)) + difference(D(x - 1, y - 1), D(x - 1, y)) +
                   difference(D(x - 1, y), D(x - 1, y + 1)) + difference(D(x, y - 1), D(x, y)) +
                   difference(D(x, y), D(x, y + 1));
            across = difference(D(x - 2, y), D(x - 1, y)) + difference(D(x - 1, y), D(x, y)) +
                     difference(D(x, y), D(x + 1, y)) + difference(P(x - 1, y), P(x, y)) +
                     difference(P(x, y), P(x + 1, y)) + difference(P(x - 1, y + 1), P(x, y + 1)) +
                     difference(P(x, y + 1), P(x + 1, y + 1));
            out.at(2 * x, 2 * y + 1) = choose(cubic(P(x, y - 1), P(x, y), P(x, y + 1), P(x, y + 2)), down,
                                              cubic(D(x - 2, y), D(x - 1, y), D(x, y), D(x + 1, y)), across, P(x, y),
                                              P(x, y + 1), D(x - 1, y), D(x, y));
        }
    }
    return out;
}

Image half(const Image& in) {
    Image out;
    out.width = std::max(1, in.width / 2), out.height = std::max(1, in.height / 2);
    out.pixels.resize(static_cast<size_t>(out.width) * out.height);
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            out.at(x, y) = (in.get(2 * x, 2 * y) + in.get(2 * x + 1, 2 * y) + in.get(2 * x, 2 * y + 1) +
                            in.get(2 * x + 1, 2 * y + 1)) * 0.25f;
        }
    }
    return out;
}

TextureImage to_bgra(const Image& image) {
    TextureImage out;
    out.width = static_cast<uint32_t>(image.width), out.height = static_cast<uint32_t>(image.height);
    out.layout = TextureLayout::Bgra8;
    out.row_pitch = out.width * 4;
    out.data.resize(static_cast<size_t>(out.row_pitch) * out.height);
    for (size_t i = 0; i < image.pixels.size(); ++i) {
        const Pixel& p = image.pixels[i];
        const float a = std::clamp(p.a, 0.0f, 1.0f), k = a > 0 ? 1 / a : 0;
        const auto byte = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
        out.data[i * 4 + 0] = byte(p.b * k);
        out.data[i * 4 + 1] = byte(p.g * k);
        out.data[i * 4 + 2] = byte(p.r * k);
        out.data[i * 4 + 3] = byte(a);
    }
    return out;
}

}  // namespace

std::vector<TextureImage> upscale_2d_texture(const TextureImage& image) {
    if (image.width < 4 || image.height < 4) return {};
    const uint32_t largest = std::max(image.width, image.height);
    if (largest > 512) return {};
    Image picture;
    if (image.layout == TextureLayout::Bgra8) {
        picture.width = static_cast<int>(image.width), picture.height = static_cast<int>(image.height);
        picture.pixels.resize(static_cast<size_t>(picture.width) * picture.height);
        for (int y = 0; y < picture.height; ++y) {
            for (int x = 0; x < picture.width; ++x) {
                picture.at(x, y) = from_bgra(image.data.data() + static_cast<size_t>(y) * image.row_pitch + x * 4);
            }
        }
    } else if (image.layout == TextureLayout::Bc1 || image.layout == TextureLayout::Bc2 ||
               image.layout == TextureLayout::Bc3) {
        picture = decode_bc(image);
        soften_block_borders(picture);
    } else {
        return {};
    }
    // Four times for the small ones (icons, letters), twice for the larger (the map's tiles): about 1024 at most.
    picture = twice(picture);
    if (largest <= 128) picture = twice(picture);
    std::vector<TextureImage> levels{to_bgra(picture)};
    while (picture.width > 1 || picture.height > 1) {
        picture = half(picture);
        levels.push_back(to_bgra(picture));
    }
    return levels;
}

}  // namespace mc3::pc
