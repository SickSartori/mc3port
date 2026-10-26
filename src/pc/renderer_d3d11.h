#pragma once

// Direct3D 11 back end for the emulated NV2A. Called only from the GPU thread.

#include <windows.h>

#include <cstdint>
#include <vector>

#include "pc/nv2a_combiner.h"
#include "pc/nv2a_texture.h"

namespace mc3::pc {

// A vertex after the NV2A vertex program. The position is in pixels of the target surface, times w (x * w,
// y * w, z * w, w); the renderer maps it to clip space with the surface's real size.
struct RenderVertex {
    float position[4];
    float diffuse[4];
    float specular[4];
    float texture[4][4];
    float fog;
};

struct SurfaceState {
    uint32_t color_address;  // physical
    uint32_t zeta_address;   // physical, 0 if none
    uint32_t width;          // the whole surface: what pixel coordinates are relative to
    uint32_t height;
    uint32_t clip_x, clip_y, clip_width, clip_height;  // where draws may write (NV097_SET_SURFACE_CLIP_*)
};

struct TextureStage {
    bool enabled;
    TextureDescriptor descriptor;
    uint32_t address_mode;  // NV097_SET_TEXTURE_ADDRESS
    uint32_t filter;        // NV097_SET_TEXTURE_FILTER
};

struct DrawState {
    CombinerProgram program;
    CombinerConstants constants;
    TextureStage textures[4];
    bool blend_enable;
    uint32_t blend_source, blend_destination, blend_equation;
    float blend_color[4];
    bool depth_test;
    uint32_t depth_function;  // 0x200 + compare
    bool depth_write;
    uint32_t color_mask;  // NV097_SET_COLOR_MASK
    // NV097_SET_STENCIL_*: OpenGL compare functions (0x200 + n) and operations (KEEP 0x1E00, REPLACE 0x1E01...).
    bool stencil_test;
    uint32_t stencil_function, stencil_reference, stencil_read_mask, stencil_write_mask;
    uint32_t stencil_fail, stencil_depth_fail, stencil_pass;
    // NV097_SET_CULL_FACE_ENABLE / SET_CULL_FACE (FRONT 0x404, BACK 0x405, FRONT_AND_BACK 0x408) / SET_FRONT_FACE
    // (CW 0x900, CCW 0x901). The game relies on it for light volumes (headlights): without it the volume's front
    // and back faces both count and the light fills a solid white ellipse.
    bool cull_enable;
    uint32_t cull_face, front_face;
};

bool renderer_start(HWND window);
// Debugging: the BGRA pixel at (x, y) of the render target at `address` (reads back from the GPU: slow).
// Returns false when there is no such target.
bool renderer_probe(uint32_t address, uint32_t x, uint32_t y, uint8_t bgra[4]);
// Debugging: saves the render target at `address` as <prefix>-<frame>.bmp (colour and alpha side by side).
void renderer_dump_target(uint32_t address, const char* prefix);
// Time spent producing vertices (vertex program / fixed function), for the statistics logged every 10 s.
void renderer_note_vertex_work(uint64_t ticks, uint32_t vertices);
void renderer_clear(const SurfaceState& surface, uint32_t flags, uint32_t color, uint32_t depth_stencil, int x0, int y0,
                    int x1, int y1);
void renderer_draw(const SurfaceState& surface, const DrawState& state, const std::vector<RenderVertex>& triangles);
// A rectangle copy between two surfaces (NV09F image blit, SRCCOPY). Addresses are physical.
struct BlitRequest {
    uint32_t source, destination;
    uint32_t source_pitch, destination_pitch, bytes_per_pixel;
    uint32_t source_x, source_y, destination_x, destination_y;
    uint32_t width, height;
};
// Between render targets the copy stays on the GPU; otherwise guest memory is copied (and uploaded into the
// destination render target, if there is one).
void renderer_blit(const BlitRequest& blit);

// Shows the surface at `address`; falls back to the guest memory there when nothing was rendered to it.
void renderer_present(uint32_t address, uint32_t width, uint32_t height, uint32_t pitch);

// F10: the modern effects (mc3port.ini) off and on, to compare with the original picture.
void renderer_toggle_effects();

}  // namespace mc3::pc
