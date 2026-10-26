#include "pc/nv2a_vsh.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace mc3::pc {
namespace {

enum Mac : uint8_t { kMacNop, kMacMov, kMacMul, kMacAdd, kMacMad, kMacDp3, kMacDph, kMacDp4, kMacDst, kMacMin, kMacMax,
                     kMacSlt, kMacSge, kMacArl };
enum Ilu : uint8_t { kIluNop, kIluMov, kIluRcp, kIluRcc, kIluRsq, kIluExp, kIluLog, kIluLit };

constexpr uint8_t kMuxTemp = 1;
constexpr uint8_t kMuxInput = 2;
constexpr uint8_t kMuxConstant = 3;
constexpr uint32_t kTemps = 12;  // r0..r11; r12 is oPos

uint32_t field(const uint32_t* instruction, uint32_t dword, uint32_t start, uint32_t length) {
    return (instruction[dword] >> start) & ((1u << length) - 1);
}

// Constant fields encode -96..95 in 8 bits; this maps them to 0..191.
uint8_t convert_constant(uint32_t c) {
    const int32_t index = static_cast<int32_t>((((c >> 5) & 7) - 3) * 32 + (c & 31)) + 96;
    return static_cast<uint8_t>(std::clamp(index, 0, 191));
}

float clamp_away_zero_inf(float value) {
    constexpr float kMin = 5.42101e-20f;
    constexpr float kMax = 1.884467e+19f;
    if (std::isnan(value)) return kMin;
    const float magnitude = std::clamp(std::fabs(value), kMin, kMax);
    return std::signbit(value) ? -magnitude : magnitude;
}

// 0 * anything is 0 on NV2A, even for infinities, which programs rely on.
float multiply(float a, float b) { return (a == 0.0f || b == 0.0f) ? 0.0f : a * b; }

}  // namespace

void VertexProgram::decode(const uint32_t* slots, uint32_t start) {
    instructions_.clear();
    key_ = 14695981039346656037ull;  // FNV-1a over the instruction words
    for (uint32_t slot = start; slot < kVshProgramSlots; ++slot) {
        const uint32_t* w = slots + slot * 4;
        for (int i = 1; i < 4; ++i) key_ = (key_ ^ w[i]) * 1099511628211ull;
        VshInstruction in{};
        in.ilu = static_cast<uint8_t>(field(w, 1, 25, 3));
        in.mac = static_cast<uint8_t>(field(w, 1, 21, 4));
        in.constant = convert_constant(field(w, 1, 13, 8));
        in.input = static_cast<uint8_t>(field(w, 1, 9, 4));

        in.a.negate = field(w, 1, 8, 1) != 0;
        in.a.swizzle[0] = static_cast<uint8_t>(field(w, 1, 6, 2));
        in.a.swizzle[1] = static_cast<uint8_t>(field(w, 1, 4, 2));
        in.a.swizzle[2] = static_cast<uint8_t>(field(w, 1, 2, 2));
        in.a.swizzle[3] = static_cast<uint8_t>(field(w, 1, 0, 2));
        in.a.reg = static_cast<uint8_t>(field(w, 2, 28, 4));
        in.a.mux = static_cast<uint8_t>(field(w, 2, 26, 2));

        in.b.negate = field(w, 2, 25, 1) != 0;
        in.b.swizzle[0] = static_cast<uint8_t>(field(w, 2, 23, 2));
        in.b.swizzle[1] = static_cast<uint8_t>(field(w, 2, 21, 2));
        in.b.swizzle[2] = static_cast<uint8_t>(field(w, 2, 19, 2));
        in.b.swizzle[3] = static_cast<uint8_t>(field(w, 2, 17, 2));
        in.b.reg = static_cast<uint8_t>(field(w, 2, 13, 4));
        in.b.mux = static_cast<uint8_t>(field(w, 2, 11, 2));

        in.c.negate = field(w, 2, 10, 1) != 0;
        in.c.swizzle[0] = static_cast<uint8_t>(field(w, 2, 8, 2));
        in.c.swizzle[1] = static_cast<uint8_t>(field(w, 2, 6, 2));
        in.c.swizzle[2] = static_cast<uint8_t>(field(w, 2, 4, 2));
        in.c.swizzle[3] = static_cast<uint8_t>(field(w, 2, 2, 2));
        in.c.reg = static_cast<uint8_t>((field(w, 2, 0, 2) << 2) | field(w, 3, 30, 2));
        in.c.mux = static_cast<uint8_t>(field(w, 3, 28, 2));

        in.mac_mask = static_cast<uint8_t>(field(w, 3, 24, 4));
        in.temp = static_cast<uint8_t>(field(w, 3, 20, 4));
        in.ilu_mask = static_cast<uint8_t>(field(w, 3, 16, 4));
        in.out_mask = static_cast<uint8_t>(field(w, 3, 12, 4));
        in.out_to_output = field(w, 3, 11, 1) != 0;
        in.out_address = static_cast<uint8_t>(field(w, 3, 3, 8));
        in.out_from_ilu = field(w, 3, 2, 1) != 0;
        in.relative = field(w, 3, 1, 1) != 0;
        in.final = field(w, 3, 0, 1) != 0;
        instructions_.push_back(in);
        if (in.final) break;
    }
}

void VertexProgram::execute(const Vec4* inputs, const Vec4* constants, Vec4* outputs) const {
    Vec4 temps[kTemps] = {};
    int32_t a0 = 0;
    for (uint32_t i = 0; i < kVshOutputs; ++i) outputs[i] = Vec4{0, 0, 0, 1};

    const auto read = [&](const VshInstruction& in, const VshInstruction::Operand& op) {
        Vec4 source{};
        switch (op.mux) {
            case kMuxTemp: source = op.reg == 12 ? outputs[kOutPosition] : (op.reg < kTemps ? temps[op.reg] : Vec4{}); break;
            case kMuxInput: source = inputs[in.input]; break;
            case kMuxConstant: source = constants[std::clamp<int32_t>(in.constant + (in.relative ? a0 : 0), 0, 191)]; break;
            default: break;
        }
        Vec4 value{source[op.swizzle[0]], source[op.swizzle[1]], source[op.swizzle[2]], source[op.swizzle[3]]};
        if (op.negate) {
            for (float& v : value) v = -v;
        }
        return value;
    };
    const auto write = [](Vec4& destination, const Vec4& value, uint8_t mask) {
        for (int component = 0; component < 4; ++component) {
            if (mask & (8 >> component)) destination[component] = value[component];
        }
    };

    for (const VshInstruction& in : instructions_) {
        const Vec4 a = read(in, in.a);
        const Vec4 b = read(in, in.b);
        const Vec4 c = read(in, in.c);

        Vec4 mac{};
        switch (in.mac) {
            case kMacMov: mac = a; break;
            case kMacMul: for (int i = 0; i < 4; ++i) mac[i] = multiply(a[i], b[i]); break;
            case kMacAdd: for (int i = 0; i < 4; ++i) mac[i] = a[i] + c[i]; break;
            case kMacMad: for (int i = 0; i < 4; ++i) mac[i] = multiply(a[i], b[i]) + c[i]; break;
            case kMacDp3: mac.fill(multiply(a[0], b[0]) + multiply(a[1], b[1]) + multiply(a[2], b[2])); break;
            case kMacDph: mac.fill(multiply(a[0], b[0]) + multiply(a[1], b[1]) + multiply(a[2], b[2]) + b[3]); break;
            case kMacDp4:
                mac.fill(multiply(a[0], b[0]) + multiply(a[1], b[1]) + multiply(a[2], b[2]) + multiply(a[3], b[3]));
                break;
            case kMacDst: mac = Vec4{1.0f, multiply(a[1], b[1]), a[2], b[3]}; break;
            case kMacMin: for (int i = 0; i < 4; ++i) mac[i] = std::min(a[i], b[i]); break;
            case kMacMax: for (int i = 0; i < 4; ++i) mac[i] = std::max(a[i], b[i]); break;
            case kMacSlt: for (int i = 0; i < 4; ++i) mac[i] = a[i] < b[i] ? 1.0f : 0.0f; break;
            case kMacSge: for (int i = 0; i < 4; ++i) mac[i] = a[i] >= b[i] ? 1.0f : 0.0f; break;
            default: break;
        }

        Vec4 ilu{};
        const float s = c[0];  // the scalar unit reads C's first selected component
        switch (in.ilu) {
            case kIluMov: ilu = c; break;
            case kIluRcp: ilu.fill(1.0f / s); break;
            case kIluRcc: ilu.fill(clamp_away_zero_inf(1.0f / s)); break;
            case kIluRsq:
                ilu.fill(s == 0.0f ? std::numeric_limits<float>::infinity()
                                   : (std::isinf(s) ? 0.0f : 1.0f / std::sqrt(std::fabs(s))));
                break;
            case kIluExp: {
                const float f = std::floor(s);
                ilu = Vec4{std::exp2(f), s - f, std::exp2(s), 1.0f};
                break;
            }
            case kIluLog: {
                const float t = std::fabs(s);
                if (t == 0.0f) {
                    const float inf = std::numeric_limits<float>::infinity();
                    ilu = Vec4{-inf, 1.0f, -inf, 1.0f};
                } else {
                    const float e = std::floor(std::log2(t));
                    ilu = Vec4{e, t / std::exp2(e), std::log2(t), 1.0f};
                }
                break;
            }
            case kIluLit: {
                constexpr float kEpsilon = 1.0f / 256.0f;
                const float x = std::max(c[0], 0.0f);
                const float y = std::max(c[1], 0.0f);
                const float w = std::clamp(c[3], -(128.0f - kEpsilon), 128.0f - kEpsilon);
                ilu = Vec4{1.0f, x, x > 0.0f ? std::exp2(w * std::log2(y)) : 0.0f, 1.0f};
                break;
            }
            default: break;
        }

        // Writes happen after both units have read their inputs.
        const bool both = in.mac != kMacNop && in.ilu != kIluNop;
        if (in.mac == kMacArl) {
            a0 = static_cast<int32_t>(std::floor(a[0] + 0.001f));
        } else if (in.mac != kMacNop && in.mac_mask) {
            if (in.temp == 12) {
                write(outputs[kOutPosition], mac, in.mac_mask);
            } else if (in.temp < kTemps && !(both && in.temp == 1)) {
                write(temps[in.temp], mac, in.mac_mask);
            }
        }
        if (in.ilu != kIluNop && in.ilu_mask) {
            const uint8_t destination = both ? 1 : in.temp;  // paired with a MAC op, the ILU writes r1
            if (destination == 12) {
                write(outputs[kOutPosition], ilu, in.ilu_mask);
            } else if (destination < kTemps) {
                write(temps[destination], ilu, in.ilu_mask);
            }
        }
        if (in.out_mask && in.out_to_output) {
            const Vec4& value = in.out_from_ilu ? ilu : mac;
            write(outputs[in.out_address & 0xF], value, in.out_mask);
        }
        if (in.final) break;
    }
}

std::string VertexProgram::hlsl() const {
    std::string s;
    char line[256];
    const auto add = [&](const char* format, auto... values) {
        std::snprintf(line, sizeof(line), format, values...);
        s += line;
    };
    const auto mask = [](uint8_t bits) {
        std::string m;
        for (int component = 0; component < 4; ++component) {
            if (bits & (8 >> component)) m += "xyzw"[component];
        }
        return m;
    };
    const auto operand = [&](const VshInstruction& in, const VshInstruction::Operand& op) {
        std::string source;
        switch (op.mux) {
            case kMuxTemp:
                source = op.reg == 12 ? "o0" : op.reg < kTemps ? "r" + std::to_string(op.reg) : "float4(0, 0, 0, 0)";
                break;
            case kMuxInput: source = "v" + std::to_string(in.input); break;
            case kMuxConstant:
                source = in.relative ? "c[clamp(" + std::to_string(in.constant) + " + a0, 0, 191)]"
                                     : "c[" + std::to_string(in.constant) + "]";
                break;
            default: source = "float4(0, 0, 0, 0)"; break;
        }
        std::string swizzle = ".";
        for (uint8_t component : op.swizzle) swizzle += "xyzw"[component];
        return (op.negate ? "-" : "") + source + swizzle;
    };

    uint32_t inputs = 0;
    for (const VshInstruction& in : instructions_) {
        for (const VshInstruction::Operand* op : {&in.a, &in.b, &in.c}) {
            if (op->mux == kMuxInput) inputs |= 1u << in.input;
        }
    }
    s += "PSInput main(uint vertex : SV_VertexID) {\n";
    for (uint32_t i = 0; i < kVshAttributes; ++i) {
        if (inputs & (1u << i)) add("    const float4 v%u = attribute(vertex, %u);\n", i, i);
    }
    s += "    float4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0, r8 = 0, r9 = 0, r10 = 0, r11 = 0;\n";
    for (uint32_t i = 0; i < kVshOutputs; ++i) add("    float4 o%u = float4(0, 0, 0, 1);\n", i);
    s += "    int a0 = 0;\n";

    for (const VshInstruction& in : instructions_) {
        s += "    {\n";
        s += "        const float4 A = " + operand(in, in.a) + ";\n";
        s += "        const float4 B = " + operand(in, in.b) + ";\n";
        s += "        const float4 C = " + operand(in, in.c) + ";\n";
        s += "        const float4 P = mul0(A, B);\n";
        const char* mac = "float4(0, 0, 0, 0)";
        switch (in.mac) {
            case kMacMov: mac = "A"; break;
            case kMacMul: mac = "P"; break;
            case kMacAdd: mac = "A + C"; break;
            case kMacMad: mac = "P + C"; break;
            case kMacDp3: mac = "(P.x + P.y + P.z).xxxx"; break;
            case kMacDph: mac = "(P.x + P.y + P.z + B.w).xxxx"; break;
            case kMacDp4: mac = "(P.x + P.y + P.z + P.w).xxxx"; break;
            case kMacDst: mac = "float4(1, P.y, A.z, B.w)"; break;
            case kMacMin: mac = "min(A, B)"; break;
            case kMacMax: mac = "max(A, B)"; break;
            case kMacSlt: mac = "float4(A < B)"; break;
            case kMacSge: mac = "float4(A >= B)"; break;
            default: break;
        }
        const char* ilu = "float4(0, 0, 0, 0)";
        switch (in.ilu) {
            case kIluMov: ilu = "C"; break;
            case kIluRcp: ilu = "(1 / C.x).xxxx"; break;
            case kIluRcc: ilu = "rcc(C.x)"; break;
            case kIluRsq: ilu = "rsq(C.x)"; break;
            case kIluExp: ilu = "exp_ilu(C.x)"; break;
            case kIluLog: ilu = "log_ilu(C.x)"; break;
            case kIluLit: ilu = "lit(C)"; break;
            default: break;
        }
        add("        const float4 M = %s;\n", mac);
        add("        const float4 I = %s;\n", ilu);
        // Writes happen after both units have read their inputs (the operands above are copies).
        const bool both = in.mac != kMacNop && in.ilu != kIluNop;
        const auto temp = [](uint32_t index) {
            return index == 12 ? std::string("o0") : index < kTemps ? "r" + std::to_string(index) : std::string();
        };
        if (in.mac == kMacArl) {
            s += "        a0 = (int)floor(A.x + 0.001);\n";
        } else if (in.mac != kMacNop && in.mac_mask && !(both && in.temp == 1)) {
            const std::string destination = temp(in.temp);
            if (!destination.empty()) {
                const std::string m = mask(in.mac_mask);
                add("        %s.%s = M.%s;\n", destination.c_str(), m.c_str(), m.c_str());
            }
        }
        if (in.ilu != kIluNop && in.ilu_mask) {
            const std::string destination = temp(both ? 1 : in.temp);  // paired with a MAC op, the ILU writes r1
            if (!destination.empty()) {
                const std::string m = mask(in.ilu_mask);
                add("        %s.%s = I.%s;\n", destination.c_str(), m.c_str(), m.c_str());
            }
        }
        if (in.out_mask && in.out_to_output) {
            const std::string m = mask(in.out_mask);
            add("        o%u.%s = %s.%s;\n", in.out_address & 0xF, m.c_str(), in.out_from_ilu ? "I" : "M", m.c_str());
        }
        s += "    }\n";
    }

    s += "    PSInput o;\n"
         "    o.position = clip_position(o0, clip_max);\n"
         "    o.d0 = o3;\n"
         "    o.d1 = o4;\n"
         "    o.t0 = o9;\n"
         "    o.t1 = o10;\n"
         "    o.t2 = o11;\n"
         "    o.t3 = o12;\n"
         "    o.fog = fog_factor(o5.x, fog_enabled != 0, fog_mode, fog_params);\n"
         "    return o;\n"
         "}\n";
    return s;
}

}  // namespace mc3::pc
