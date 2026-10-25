#pragma once

// NV2A register combiners (the fixed "pixel shader" hardware) translated to HLSL.

#include <cstdint>
#include <string>

namespace mc3::pc {

// The structural part of the pixel pipeline: everything that changes the generated code. Constants (factors,
// alpha reference) are passed separately.
struct CombinerProgram {
    uint32_t color_input[8];
    uint32_t color_output[8];
    uint32_t alpha_input[8];
    uint32_t alpha_output[8];
    uint32_t control;  // stage count, mux select, unique constants
    uint32_t final0;   // final combiner A, B, C, D
    uint32_t final1;   // final combiner E, F, G and flags
    uint32_t stage_program;  // texture mode per stage, 5 bits each
    uint32_t alpha_test;     // 0 = off, else 0x200 + function (NV097_SET_ALPHA_FUNC)

    bool operator==(const CombinerProgram& other) const;
};

// Constant buffer layout shared with the generated shader (register b0).
struct CombinerConstants {
    float factor0[8][4];
    float factor1[8][4];
    float final_factor0[4];
    float final_factor1[4];
    float fog_color[4];
    float alpha_reference[4];  // x = reference 0..1
    float texture_scale[4][4];  // xy multiply the coordinates: 1/size for rectangle (linear) textures, else 1
};

uint64_t combiner_hash(const CombinerProgram& program);
std::string generate_pixel_shader(const CombinerProgram& program);

// D3DCOLOR (ARGB) to float RGBA.
void unpack_color(uint32_t argb, float out[4]);

}  // namespace mc3::pc
