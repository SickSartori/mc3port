#pragma once

// Direct3D 11 back end for the emulated NV2A. Called only from the GPU thread.

#include <windows.h>

#include <cstdint>
#include <vector>

#include "pc/nv2a_combiner.h"
#include "pc/nv2a_texture.h"

namespace mc3::pc {

// A vertex after the NV2A vertex program, position already in clip space for D3D11.
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
    uint32_t width;
    uint32_t height;
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
};

bool renderer_start(HWND window);
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

}  // namespace mc3::pc
