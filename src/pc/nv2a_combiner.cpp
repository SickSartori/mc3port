#include "pc/nv2a_combiner.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace mc3::pc {
namespace {

// Register numbers (PS_REGISTER_*).
constexpr uint32_t kRegZero = 0x0, kRegC0 = 0x1, kRegC1 = 0x2, kRegFog = 0x3, kRegV0 = 0x4, kRegV1 = 0x5;
constexpr uint32_t kRegT0 = 0x8, kRegR0 = 0xC, kRegR1 = 0xD, kRegV1R0Sum = 0xE, kRegEfProduct = 0xF;
constexpr uint32_t kChannelAlpha = 0x10;

// COMBINER_CONTROL flags (above the stage count).
constexpr uint32_t kMuxMsb = 1u << 8;
constexpr uint32_t kUniqueC0 = 1u << 12;
constexpr uint32_t kUniqueC1 = 1u << 16;

// Final combiner flags (low byte of final1).
constexpr uint32_t kClampSum = 0x80, kComplementV1 = 0x40, kComplementR0 = 0x20;

// Texture modes (PS_TEXTUREMODES_*).
constexpr uint32_t kModeNone = 0x0, kModeProject2d = 0x1, kModeProject3d = 0x2, kModePassthrough = 0x4;

std::string format(const char* text, ...) {
    char buffer[512];
    va_list args;
    va_start(args, text);
    std::vsnprintf(buffer, sizeof(buffer), text, args);
    va_end(args);
    return buffer;
}

std::string register_name(uint32_t reg, int stage) {
    switch (reg) {
        case kRegZero: return "float4(0, 0, 0, 0)";
        case kRegC0: return stage < 0 ? "final_factor0" : format("factor0[%d]", stage);
        case kRegC1: return stage < 0 ? "final_factor1" : format("factor1[%d]", stage);
        case kRegFog: return "float4(fog_color.rgb, input.fog)";
        case kRegV0: return "v0";
        case kRegV1: return "v1";
        case kRegT0: return "t0";
        case kRegT0 + 1: return "t1";
        case kRegT0 + 2: return "t2";
        case kRegT0 + 3: return "t3";
        case kRegR0: return "r0";
        case kRegR1: return "r1";
        case kRegV1R0Sum: return "v1r0sum";
        case kRegEfProduct: return "efproduct";
        default: return "float4(0, 0, 0, 0)";
    }
}

// Writable destinations; 0 means discard.
const char* destination_name(uint32_t reg) {
    switch (reg) {
        case kRegV0: return "v0";
        case kRegV1: return "v1";
        case kRegT0: return "t0";
        case kRegT0 + 1: return "t1";
        case kRegT0 + 2: return "t2";
        case kRegT0 + 3: return "t3";
        case kRegR0: return "r0";
        case kRegR1: return "r1";
        default: return nullptr;
    }
}

std::string apply_input_mapping(const std::string& x, uint32_t mapping) {
    switch (mapping & 0xE0) {
        case 0x00: return "max(" + x + ", 0)";
        case 0x20: return "(1 - saturate(" + x + "))";
        case 0x40: return "(2 * max(" + x + ", 0) - 1)";
        case 0x60: return "(-2 * max(" + x + ", 0) + 1)";
        case 0x80: return "(max(" + x + ", 0) - 0.5)";
        case 0xA0: return "(-max(" + x + ", 0) + 0.5)";
        case 0xC0: return x;
        default: return "(-" + x + ")";
    }
}

// One ICW byte as a float3 (colour) or float (alpha) expression.
std::string input(uint32_t byte, bool alpha_path, int stage) {
    const std::string reg = register_name(byte & 0xF, stage);
    std::string value;
    if (alpha_path) {
        value = reg + ((byte & kChannelAlpha) ? ".a" : ".b");
    } else {
        value = reg + ((byte & kChannelAlpha) ? ".aaa" : ".rgb");
    }
    return apply_input_mapping(value, byte);
}

std::string apply_output_mapping(const std::string& x, uint32_t flags) {
    switch (flags & 0x38) {
        case 0x08: return "(" + x + " - 0.5)";
        case 0x10: return "(" + x + " * 2)";
        case 0x18: return "((" + x + " - 0.5) * 2)";
        case 0x20: return "(" + x + " * 4)";
        case 0x30: return "(" + x + " / 2)";
        default: return x;
    }
}

void generate_stage(std::string& code, const CombinerProgram& p, int stage) {
    const uint32_t ci = p.color_input[stage], co = p.color_output[stage];
    const uint32_t ai = p.alpha_input[stage], ao = p.alpha_output[stage];
    const char* select = (p.control & kMuxMsb) ? "(r0.a >= 0.5)" : "((uint(r0.a * 255) & 1) != 0)";
    const int c0_stage = (p.control & kUniqueC0) ? stage : 0;
    const int c1_stage = (p.control & kUniqueC1) ? stage : 0;
    // Constants of shared-constant stages come from stage 0.
    auto stage_input = [&](uint32_t byte, bool alpha) {
        const uint32_t reg = byte & 0xF;
        return input(byte, alpha, reg == kRegC0 ? c0_stage : reg == kRegC1 ? c1_stage : stage);
    };

    code += format("    { // stage %d\n", stage);
    // Colour.
    const uint32_t cflags = co >> 12;
    code += "        float3 A = " + stage_input(ci >> 24, false) + ";\n";
    code += "        float3 B = " + stage_input((ci >> 16) & 0xFF, false) + ";\n";
    code += "        float3 C = " + stage_input((ci >> 8) & 0xFF, false) + ";\n";
    code += "        float3 D = " + stage_input(ci & 0xFF, false) + ";\n";
    code += std::string("        float3 ab = ") + ((cflags & 2) ? "dot(A, B).xxx" : "A * B") + ";\n";
    code += std::string("        float3 cd = ") + ((cflags & 1) ? "dot(C, D).xxx" : "C * D") + ";\n";
    code += std::string("        float3 sum = ") + ((cflags & 4) ? std::string(select) + " ? cd : ab" : "ab + cd") + ";\n";
    code += "        ab = clamp(" + apply_output_mapping("ab", cflags) + ", -1, 1);\n";
    code += "        cd = clamp(" + apply_output_mapping("cd", cflags) + ", -1, 1);\n";
    code += "        sum = clamp(" + apply_output_mapping("sum", cflags) + ", -1, 1);\n";
    // Alpha.
    const uint32_t aflags = ao >> 12;
    code += "        float a = " + stage_input(ai >> 24, true) + ";\n";
    code += "        float b = " + stage_input((ai >> 16) & 0xFF, true) + ";\n";
    code += "        float c = " + stage_input((ai >> 8) & 0xFF, true) + ";\n";
    code += "        float d = " + stage_input(ai & 0xFF, true) + ";\n";
    code += "        float ab_a = a * b;\n";
    code += "        float cd_a = c * d;\n";
    code += std::string("        float sum_a = ") + ((aflags & 4) ? std::string(select) + " ? cd_a : ab_a" : "ab_a + cd_a") + ";\n";
    code += "        ab_a = clamp(" + apply_output_mapping("ab_a", aflags) + ", -1, 1);\n";
    code += "        cd_a = clamp(" + apply_output_mapping("cd_a", aflags) + ", -1, 1);\n";
    code += "        sum_a = clamp(" + apply_output_mapping("sum_a", aflags) + ", -1, 1);\n";

    // Writes: alpha, then colour, then blue-to-alpha (which overrides the alpha written to that register).
    if (const char* dst = destination_name((ao >> 4) & 0xF)) code += format("        %s.a = ab_a;\n", dst);
    if (const char* dst = destination_name(ao & 0xF)) code += format("        %s.a = cd_a;\n", dst);
    if (const char* dst = destination_name((ao >> 8) & 0xF)) code += format("        %s.a = sum_a;\n", dst);
    if (const char* dst = destination_name((co >> 4) & 0xF)) code += format("        %s.rgb = ab;\n", dst);
    if (const char* dst = destination_name(co & 0xF)) code += format("        %s.rgb = cd;\n", dst);
    if (const char* dst = destination_name((co >> 8) & 0xF)) code += format("        %s.rgb = sum;\n", dst);
    if (cflags & 0x80) {
        if (const char* dst = destination_name((co >> 4) & 0xF)) code += format("        %s.a = ab.b;\n", dst);
    }
    if (cflags & 0x40) {
        if (const char* dst = destination_name(co & 0xF)) code += format("        %s.a = cd.b;\n", dst);
    }
    code += "    }\n";
}

std::string texture_fetch(uint32_t mode, int stage) {
    switch (mode) {
        case kModeNone: return "float4(0, 0, 0, 0)";
        case kModeProject2d:
        case kModeProject3d:
            return format(
                "texture%d.Sample(sampler%d, input.t%d.xy / (input.t%d.w != 0 ? input.t%d.w : 1) * texture_scale[%d].xy)",
                stage, stage, stage, stage, stage, stage);
        case kModePassthrough: return format("saturate(input.t%d)", stage);
        default: return "float4(0, 0, 0, 0)";  // cube maps, bump mapping and dot-product modes: not yet
    }
}

}  // namespace

bool CombinerProgram::operator==(const CombinerProgram& other) const {
    return std::memcmp(this, &other, sizeof(*this)) == 0;
}

uint64_t combiner_hash(const CombinerProgram& program) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&program);
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof(program); ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}

std::string generate_pixel_shader(const CombinerProgram& p) {
    std::string code =
        "struct PSInput {\n"
        "    float4 position : SV_Position;\n"
        "    float4 d0 : COLOR0;\n"
        "    float4 d1 : COLOR1;\n"
        "    float4 t0 : TEXCOORD0;\n"
        "    float4 t1 : TEXCOORD1;\n"
        "    float4 t2 : TEXCOORD2;\n"
        "    float4 t3 : TEXCOORD3;\n"
        "    float fog : FOG;\n"
        "};\n"
        "cbuffer Constants : register(b0) {\n"
        "    float4 factor0[8];\n"
        "    float4 factor1[8];\n"
        "    float4 final_factor0;\n"
        "    float4 final_factor1;\n"
        "    float4 fog_color;\n"
        "    float4 alpha_reference;\n"
        "    float4 texture_scale[4];\n"
        "};\n";
    for (int i = 0; i < 4; ++i) {
        code += format("Texture2D texture%d : register(t%d);\nSamplerState sampler%d : register(s%d);\n", i, i, i, i);
    }
    code += "float4 main(PSInput input) : SV_Target {\n";
    code += "    float4 v0 = saturate(input.d0);\n";
    code += "    float4 v1 = saturate(input.d1);\n";
    for (int i = 0; i < 4; ++i) {
        code += format("    float4 t%d = %s;\n", i, texture_fetch((p.stage_program >> (i * 5)) & 0x1F, i).c_str());
    }
    code += std::string("    float4 r0 = float4(0, 0, 0, ") + (((p.stage_program & 0x1F) != kModeNone) ? "t0.a" : "1") + ");\n";
    code += "    float4 r1 = float4(0, 0, 0, 0);\n";

    const uint32_t stages = std::min<uint32_t>(p.control & 0xFF, 8);
    for (uint32_t stage = 0; stage < stages; ++stage) generate_stage(code, p, static_cast<int>(stage));

    if (p.final0 == 0 && p.final1 == 0) {
        code += "    float4 result = r0;\n";
    } else {
        const uint32_t flags = p.final1 & 0xFF;
        code += std::string("    float4 v1r0sum = float4(") + ((flags & kComplementV1) ? "(1 - v1.rgb)" : "v1.rgb") + " + " +
                ((flags & kComplementR0) ? "(1 - r0.rgb)" : "r0.rgb") + ", 0);\n";
        if (flags & kClampSum) code += "    v1r0sum = saturate(v1r0sum);\n";
        code += "    float4 efproduct = float4(" + input(p.final1 >> 24, false, -1) + " * " +
                input((p.final1 >> 16) & 0xFF, false, -1) + ", 0);\n";
        code += "    float3 fa = " + input(p.final0 >> 24, false, -1) + ";\n";
        code += "    float3 fb = " + input((p.final0 >> 16) & 0xFF, false, -1) + ";\n";
        code += "    float3 fc = " + input((p.final0 >> 8) & 0xFF, false, -1) + ";\n";
        code += "    float3 fd = " + input(p.final0 & 0xFF, false, -1) + ";\n";
        code += "    float4 result = float4(fa * fb + (1 - fa) * fc + fd, " + input((p.final1 >> 8) & 0xFF, true, -1) + ");\n";
    }
    code += "    result = saturate(result);\n";

    if (p.alpha_test) {
        static const char* const kCompare[8] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
        const uint32_t function = p.alpha_test & 7;
        code += "    int alpha = int(round(result.a * 255));\n";
        code += "    int reference = int(round(alpha_reference.x * 255));\n";
        if (function == 0) {
            code += "    discard;\n";
        } else if (function != 7) {
            code += format("    if (!(alpha %s reference)) discard;\n", kCompare[function]);
        }
    }
    code += "    return result;\n}\n";
    return code;
}

void unpack_color(uint32_t argb, float out[4]) {
    out[0] = ((argb >> 16) & 0xFF) / 255.0f;
    out[1] = ((argb >> 8) & 0xFF) / 255.0f;
    out[2] = (argb & 0xFF) / 255.0f;
    out[3] = (argb >> 24) / 255.0f;
}

}  // namespace mc3::pc
