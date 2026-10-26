#pragma once

// NV2A vertex programs ("transform programs"), executed on the CPU.
//
// Each instruction is 4 dwords (the first unused) and pairs a MAC operation (vector math) with an ILU operation
// (scalar functions); see docs/ANALISI_XBE.md for the field layout. Programs output the position already in screen
// space (the Xbox D3D appends the viewport transform), plus colours, fog and texture coordinates.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace mc3::pc {

constexpr uint32_t kVshProgramSlots = 136;
constexpr uint32_t kVshConstants = 192;
constexpr uint32_t kVshAttributes = 16;
constexpr uint32_t kVshOutputs = 16;

// Output register indices.
enum VshOutput : uint32_t {
    kOutPosition = 0,
    kOutDiffuse = 3,
    kOutSpecular = 4,
    kOutFog = 5,
    kOutPointSize = 6,
    kOutBackDiffuse = 7,
    kOutBackSpecular = 8,
    kOutTexture0 = 9,
};

using Vec4 = std::array<float, 4>;

struct VshInstruction {
    uint8_t ilu, mac;
    uint8_t constant;  // converted to 0..191
    uint8_t input;     // v register
    struct Operand {
        uint8_t mux;  // 1 = temp, 2 = input, 3 = constant
        uint8_t reg;
        bool negate;
        uint8_t swizzle[4];
    } a, b, c;
    uint8_t mac_mask, ilu_mask, out_mask;  // bit 3 = x ... bit 0 = w
    uint8_t temp;
    bool out_to_output;  // ORB: output register (true) or constant (false)
    uint8_t out_address;
    bool out_from_ilu;
    bool relative;  // constant indexed by A0
    bool final;
};

class VertexProgram {
public:
    // Decodes from `start` up to the instruction marked final.
    void decode(const uint32_t* slots, uint32_t start);
    bool empty() const { return instructions_.empty(); }
    // The same instruction words give the same key: for caching the program's translation.
    uint64_t key() const { return key_; }

    void execute(const Vec4* inputs, const Vec4* constants, Vec4* outputs) const;
    // The program as the main function of an HLSL vertex shader doing what execute does, for the renderer
    // (which supplies the helpers and the constants: kVertexProgramShader in renderer_d3d11.cpp).
    std::string hlsl() const;

private:
    std::vector<VshInstruction> instructions_;
    uint64_t key_ = 0;
};

}  // namespace mc3::pc
