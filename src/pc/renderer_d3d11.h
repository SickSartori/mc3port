#pragma once

// Direct3D 11 back end for the emulated NV2A. Called only from the GPU thread.

#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <functional>
#include <string>
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

// Fixed-function vertex transform done on the GPU: the state the CPU version (nv2a_kelvin.cpp) reads, laid out as
// float4 rows for the shader's constant buffer. Matrices are rows dotted with the input vector.
struct alignas(16) FixedFunctionConstants {
    float composite[4][4];
    float model_view[4][4];
    float inverse_model_view[4][4];
    float texture_matrix[4][4][4];      // per stage
    float texgen_plane[4][4][4];        // per stage, S T R Q
    int32_t texgen_mode[4][4];          // per stage, S T R Q: 0 none, 1 eye linear, 2 object linear
    float attribute_default[16][4];     // attributes without an array
    // x: byte offset of vertex 0's attribute inside the vertex data (may be negative: only the indices used are
    // uploaded), y: stride, z: NV097_SET_VERTEX_DATA_ARRAY_FORMAT type | size << 4, w: 1 = from the data, 0 = default
    int32_t attribute_layout[16][4];
    float light[8][32];                 // NV097_SET_LIGHT_* blocks
    float viewport_offset[4];
    float eye_position[4];
    float scene_ambient[4];
    float material_emission[4];         // w: material alpha
    float specular_params[8];
    float fog_plane[4];
    float fog_params[4];
    uint32_t flags;                     // kFixedFunction* below
    uint32_t color_material, light_enable_mask, fog_gen_mode;
    uint32_t fog_mode, texture_matrix_mask, padding;
    float clip_max;
};
constexpr uint32_t kFixedFunctionLighting = 1, kFixedFunctionSpecular = 2, kFixedFunctionFog = 4,
                   kFixedFunctionNormalize = 8, kFixedFunctionSeparateSpecular = 16, kFixedFunctionLocalEye = 32;

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
// Whether renderer_draw_fixed_function is available (shader compiled, buffers usable).
bool renderer_has_fixed_function();
// The same draw with the fixed-function transform on the GPU: `vertex_data` holds the guest vertex bytes (4-byte
// multiple) the shader decodes through constants.attribute_layout, `indices` the triangle list of vertex numbers.
void renderer_draw_fixed_function(const SurfaceState& surface, const DrawState& state,
                                  const FixedFunctionConstants& constants, const void* vertex_data,
                                  uint32_t vertex_bytes, const std::vector<uint32_t>& indices);
// A vertex program on the GPU: its constant memory, and the attributes as for the fixed function.
struct alignas(16) ProgramConstants {
    float constants[192][4];
    float attribute_default[16][4];
    int32_t attribute_layout[16][4];  // as FixedFunctionConstants::attribute_layout
    float fog_params[4];
    uint32_t fog_enabled, fog_mode, padding;
    float clip_max;
};
// Makes the vertex program `key` ready on the GPU, compiling `source()` (VertexProgram::hlsl) the first time.
// False when it cannot run there (it did not compile): the CPU then runs it.
bool renderer_prepare_program(uint64_t key, const std::function<std::string()>& source);
// A draw whose vertices go through the prepared vertex program `key`; the rest as renderer_draw_fixed_function.
void renderer_draw_program(const SurfaceState& surface, const DrawState& state, uint64_t key,
                           const ProgramConstants& constants, const void* vertex_data, uint32_t vertex_bytes,
                           const std::vector<uint32_t>& indices);
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

// The shape the picture is shown in: 16:9 with the widescreen setting (the game then draws for a widescreen TV),
// otherwise 4:3.
float renderer_aspect();

// F10: the modern effects (mc3port.ini) off and on, to compare with the original picture.
void renderer_toggle_effects();
void renderer_set_effects(bool on);

// Drawn over every presented frame (the in-game menu, the frame counter), on the GPU thread, into the window's back
// buffer of `width` x `height` pixels. nullptr removes it.
using RendererOverlay = void (*)(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
                                 uint32_t width, uint32_t height);
void renderer_set_overlay(RendererOverlay overlay);

}  // namespace mc3::pc
