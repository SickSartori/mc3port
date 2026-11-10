#pragma once

// The new HUD (setting hud_style = 1). The game's dials (speedometer, rev counter, gear, the special ability's arc,
// the red bar under the speedometer) are read from their draws and left out; the port draws its own over the
// picture, sharp at any resolution. The radar stays the game's.

#include <cstdint>
#include <vector>

struct ImDrawList;
struct ImFont;

namespace xport::pc {

// A flat four-vertex draw in the lower right of a frame (nv2a_kelvin.cpp): positions in surface pixels, the first
// texture's coordinates, the diffuse colour of the first vertex.
struct HudQuad {
    bool program;  // a vertex program draws the game's faces; the fixed function its needles and bar
    uint32_t texture;
    float x[4], y[4], u[4], v[4];
    float colour[4];
};

bool hud_enabled();
// Whether the dials are read: for the new HUD, and for hud_aspect (renderer_d3d11.cpp narrows what lies in them).
bool hud_reading();
// The draw is one of the dials the new HUD replaces: it is then not drawn (with hud_style only).
bool hud_take_quad(const HudQuad& quad);
// Where the game's dials were in the last frame read (surface pixels): each face, the gear, the special ability's arc
// and the red bar.
struct HudBox {
    float left, top, right, bottom;
    bool wide_only = false;  // the special ability's tall track: only draws as wide as it count (not the letters of a text over it)
};
const std::vector<HudBox>& hud_boxes();
// renderer_present: what this frame's draws showed becomes what the HUD draws.
void hud_frame_shown();

// Where the game's picture is in the window (renderer_d3d11.cpp), in surface pixels and window pixels.
struct HudPicture {
    float surface_left, surface_top, surface_width, surface_height;
    float window_left, window_top, window_width, window_height;
};
struct HudFonts {
    ImFont* numbers;  // dial numbers, the gear
    ImFont* labels;   // units and captions
};
void hud_draw(ImDrawList* list, const HudFonts& fonts);

}  // namespace xport::pc
