// Midnight Club 3's part of the shared renderer (sick-xport's renderer_d3d11.cpp calls these): the HUD narrowed back to
// its shape in 16:9 (setting hud_aspect), LB and RB on the button symbols, the cars' liveries enlarged with sharp_2d,
// and the motion blur and light streaks scaled by their settings.
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <unordered_set>
#include <vector>

#include "pc/hud.h"
#include "pc/log.h"
#include "pc/renderer_d3d11.h"
#include "pc/settings.h"

namespace xport::pc {
namespace {

// Setting hud_aspect. In 16:9 MC3 draws its HUD with the pixel sizes of 4:3 on a picture 720 pixels wide that is shown
// 16:9 wide, so round things (radar, dials) come out oval. A flat draw (nv2a_kelvin.cpp) over the 3D scene on a shown
// frame that lies within the HUD is narrowed by (720 / 480) / (16 / 9) towards its side: the radar's box (the scissor
// rectangle its map is cut to, with room for the icons on its edge) towards the left, the dials hud.cpp read (each
// face, the gear, the special ability's arc, the red bar) towards the right. Nothing else: texts come in pieces, and
// narrowing the pieces near a side tore them apart (over the tutorials' films and the loading screens too, where the
// last 3D scene still counted). The scissor rectangle follows the draw; the radar's clear of the alpha does not: its
// mask then covered less than the original box, and the frame showed the previous one through the rest.
int g_debug_hud = -1;        // MC3_DEBUG_HUD=<n>: the first n decisions in the log
HudBox g_radar_box{};        // the radar's scissor rectangle...
ULONGLONG g_radar_seen = 0;  // ...last seen then

bool inside(float left, float top, float right, float bottom, const HudBox& box, float margin) {
    return left >= box.left - margin && right <= box.right + margin && top >= box.top - margin &&
           bottom <= box.bottom + margin && (!box.wide_only || right - left >= 0.8f * (box.right - box.left));
}

bool hud_aspect() {
    static const bool on = setting_at_start("hud_aspect") != 0;
    return on;
}

// The game's atlas of button symbols shows White and Black as plain discs. Here those buttons are LB and RB
// (input_hle.cpp), so the discs get those letters, drawn in code as the texture is decoded.
constexpr uint64_t kButtonAtlas = 0xA30CEDEE3C0953C1ull;

void letter_shoulder_buttons(TextureImage& atlas) {
    if (atlas.layout != TextureLayout::Bgra8 || atlas.width != 128 || atlas.height != 128) return;
    static constexpr uint8_t kLetters[3][5] = {{4, 4, 4, 4, 7}, {6, 5, 6, 5, 6}, {6, 5, 6, 5, 5}};  // L, B, R; 3 x 5
    const auto letter = [&](int which, int left, int top, uint8_t shade) {
        for (int y = 0; y < 10; ++y) {
            for (int x = 0; x < 6; ++x) {
                if (!(kLetters[which][y / 2] >> (2 - x / 2) & 1)) continue;
                uint8_t* pixel = atlas.data.data() + static_cast<size_t>(top + y) * atlas.row_pitch + (left + x) * 4;
                pixel[0] = pixel[1] = pixel[2] = shade;
                pixel[3] = 255;
            }
        }
    };
    letter(0, 87, 71, 24), letter(1, 95, 71, 24);    // the white disc, centred at 94,76: LB, dark
    letter(2, 14, 107, 240), letter(1, 22, 107, 240);  // the black disc, centred at 21,112: RB, light
}

std::unordered_set<uint32_t> g_frame_copies;  // surfaces drawn from a shown one: the speed blur's buffer

}  // namespace

// The narrowing for something from `left` to `right` and `top` to `bottom` (pixels) on `surface`; none when it is
// not HUD.
RendererNarrowing renderer_game_narrowing(const SurfaceState& surface, float left, float right, float top, float bottom,
                                          uint32_t texture) {
    RendererNarrowing n;
    const ShownPicture picture = renderer_shown_picture();
    if (!hud_aspect() || left > right || !picture.width || !picture.height) return n;
    const float width = static_cast<float>(picture.width), height = static_cast<float>(picture.height);
    const ULONGLONG now = GetTickCount64();
    // The radar's map: a small scissor rectangle in the lower left.
    if (surface.clip_width <= 200 && surface.clip_height <= 200 && surface.clip_x + surface.clip_width <= width / 2 &&
        surface.clip_y >= height / 2) {
        g_radar_box = {static_cast<float>(surface.clip_x), static_cast<float>(surface.clip_y),
                       static_cast<float>(surface.clip_x + surface.clip_width),
                       static_cast<float>(surface.clip_y + surface.clip_height)};
        g_radar_seen = now;
    }
    // What can show: the part inside the scissor rectangle (the radar's map is turning tiles, cut to its box).
    left = std::max(left, static_cast<float>(surface.clip_x));
    right = std::min(right, static_cast<float>(surface.clip_x + surface.clip_width));
    top = std::max(top, static_cast<float>(surface.clip_y));
    bottom = std::min(bottom, static_cast<float>(surface.clip_y + surface.clip_height));
    if (left > right || top > bottom) return n;
    const float narrow = width / height / renderer_aspect();
    const bool over_shown_scene = renderer_over_shown_scene(surface);
    const bool radar = now - g_radar_seen < 1000 &&
                       inside(left, top, right, bottom, g_radar_box, 0.35f * (g_radar_box.right - g_radar_box.left));
    // With mc3port's HUD the game's dials are not drawn (hud.cpp takes them), so nothing in their boxes needs the
    // narrowing, and the letters of a text that cross a box (the special ability's tall track reaches the texts
    // above the dials) were torn from the rest of their line.
    const std::vector<HudBox>& dials = hud_boxes();
    const bool dial = !hud_enabled() && std::any_of(dials.begin(), dials.end(), [&](const HudBox& box) {
                          return inside(left, top, right, bottom, box, 6);
                      });
    if (narrow < 0.99f && over_shown_scene && (radar || dial)) {
        n.anchor = radar ? 0 : width;
        n.factor = narrow;
    }
    if (g_debug_hud < 0) {
        char value[16] = {};
        g_debug_hud = GetEnvironmentVariableA("MC3_DEBUG_HUD", value, sizeof(value)) ? std::atoi(value) : 0;
    }
    if (g_debug_hud > 0) {
        --g_debug_hud;
        log_line("hud: %.0f..%.0f x %.0f..%.0f on %08X (clip %u,%u %ux%u), texture %08X: %s%s%s-> %.3f towards %.0f",
                 left, right, top, bottom, surface.color_address, surface.clip_x, surface.clip_y, surface.clip_width,
                 surface.clip_height, texture, over_shown_scene ? "" : "not over a shown scene ", radar ? "radar " : "",
                 dial ? "dial " : "", n.factor, n.anchor);
    }
    return n;
}

// The radar's clear of the alpha stays where it is while its quads are narrowed towards the left: the strip they
// gained kept the scene's alpha, and the map showed there, outside the ring. So it reaches that far too.
void renderer_game_clear(const SurfaceState& surface, uint32_t channels, int& x0, int& y0, int& x1, int& y1) {
    const ShownPicture picture = renderer_shown_picture();
    if (channels == 8 && hud_aspect() && picture.width && picture.height && x1 - x0 <= 200 && y1 - y0 <= 200 &&
        x1 <= static_cast<int>(picture.width / 2) && y0 >= static_cast<int>(picture.height / 2) &&
        renderer_over_shown_scene(surface)) {
        const float narrow = static_cast<float>(picture.width) / static_cast<float>(picture.height) / renderer_aspect();
        if (narrow < 0.99f) x0 = static_cast<int>(static_cast<float>(x0) * narrow);
    }
}

// A car's livery is one 256x256 paletted picture stretched over the whole car, the only texture of that kind seen
// in the city: close up, at more than 480 lines, its texels show as steps along the stripes.
bool renderer_game_enlarge(const TextureDescriptor& d) {
    return (d.format >> 8 & 0x7F) == 0x0B && (d.format >> 16 & 0xFFF) == 0x881;
}

bool renderer_game_texture(const TextureDescriptor& d, const uint8_t* memory, uint32_t bytes, TextureImage& image) {
    if (image.width != 128 || image.height != 128 || renderer_texture_hash(memory, bytes) != kButtonAtlas) return false;
    letter_shoulder_buttons(image);
    log_line("renderer: LB and RB drawn on the button symbols at %08X", d.address);
    return true;  // the atlas's own lower levels would keep the plain discs
}

void renderer_game_draw(const SurfaceState& surface, const DrawState& state, float alpha_reference[4]) {
    // The motion blur: the blur buffer (the surface a draw read while drawing into it) added to the frame, weighted by
    // the frame's alpha (DST_ALPHA, ONE). Setting motion_blur scales it (1 = like the Xbox, 0 = off).
    const uint32_t blur_surface = renderer_self_read_surface();
    if (blur_surface && surface.color_address != blur_surface && state.blend_enable && state.blend_source == 0x304 &&
        state.blend_destination == 1 && state.textures[0].enabled && state.textures[0].descriptor.address == blur_surface) {
        alpha_reference[1] = 1 - std::clamp(setting("motion_blur"), 0.0f, 1.0f);
    }
    // The speed blur at high speed is made another way: the shown frame is shrunk into a surface, blended there with
    // slightly enlarged copies of itself (the streaks), and laid back over the frame by its alpha (SRC_ALPHA,
    // INV_SRC_ALPHA): setting motion_blur scales it. The lights' streaks go through the same surface: their halos
    // are drawn into it, stretched the same way and added to the frame (SRC_ALPHA, ONE): setting light_streaks.
    const bool reads_frame = state.textures[0].enabled && renderer_surface_shown(state.textures[0].descriptor.address);
    const bool draws_frame = renderer_surface_shown(surface.color_address);
    if (reads_frame && !draws_frame) g_frame_copies.insert(surface.color_address);
    if (draws_frame && state.blend_enable && state.blend_source == 0x302 &&
        (state.blend_destination == 0x303 || state.blend_destination == 1) && state.textures[0].enabled &&
        g_frame_copies.count(state.textures[0].descriptor.address)) {
        const char* key = state.blend_destination == 1 ? "light_streaks" : "motion_blur";
        alpha_reference[2] = 1 - std::clamp(setting(key), 0.0f, 1.0f);
    }
}

void renderer_game_frame_shown() { hud_frame_shown(); }

}  // namespace xport::pc
