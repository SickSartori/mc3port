#include "pc/nv2a_kelvin.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <windows.h>
#include <set>
#include <vector>

#include "pc/log.h"
#include "pc/nv2a_combiner.h"
#include "pc/nv2a_texture.h"
#include "pc/nv2a_vsh.h"
#include "pc/physical_memory.h"
#include "pc/renderer_d3d11.h"

namespace mc3::pc {
namespace {

// Primitive types (NV097_SET_BEGIN_END).
enum : uint32_t { kEnd = 0, kPoints, kLines, kLineLoop, kLineStrip, kTriangles, kTriangleStrip, kTriangleFan, kQuads,
                  kQuadStrip, kPolygon };

// Vertex attribute types (NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE).
enum : uint32_t { kTypeUbD3d = 0, kTypeS1 = 1, kTypeFloat = 2, kTypeUbOgl = 4, kTypeS32k = 5, kTypeCompressed = 6 };

constexpr uint32_t kExecutionModeProgram = 2;

struct Kelvin {
    DmaResolver dma = nullptr;
    uint32_t dma_a = 0, dma_b = 0, dma_color = 0, dma_zeta = 0, dma_vertex_a = 0, dma_vertex_b = 0;

    uint32_t clip_horizontal = 0, clip_vertical = 0, surface_format = 0, surface_pitch = 0;
    uint32_t color_offset = 0, zeta_offset = 0;
    uint32_t color_clear = 0, zstencil_clear = 0, clear_horizontal = 0, clear_vertical = 0;

    uint32_t execution_mode = 0;
    uint32_t program[kVshProgramSlots * 4] = {};
    uint32_t program_load = 0, program_start = 0;
    bool program_dirty = true;
    VertexProgram vertex_program;
    Vec4 constants[kVshConstants] = {};
    uint32_t constant_load = 0;
    float clip_min = 0, clip_max = 16777215.0f;

    uint32_t array_format[kVshAttributes] = {};
    uint32_t array_offset[kVshAttributes] = {};
    Vec4 attribute[kVshAttributes] = {};

    uint32_t primitive = kEnd;
    std::vector<uint32_t> inline_data;
    std::vector<uint32_t> elements;

    DrawState draw{};
    uint32_t combiner_factor0[8] = {}, combiner_factor1[8] = {};
    bool alpha_test = false;
    uint32_t alpha_function = 0x207, alpha_reference = 0;
    uint32_t texture_control0[4] = {}, texture_control1[4] = {}, texture_rect[4] = {}, texture_palette[4] = {};
    uint32_t texture_offset[4] = {}, texture_format[4] = {};

    // Fixed-function transform. Matrices are stored like vertex program constants: element [row * 4 + column],
    // and a row is dotted with the input vector.
    float model_view[4][16] = {};
    float composite[16] = {};
    float texture_matrix[4][16] = {};
    bool texture_matrix_enable[4] = {};
    uint32_t texgen[4][4] = {};          // per stage, S T R Q
    float texgen_plane[4][4][4] = {};    // per stage, S T R Q
    float viewport_offset[4] = {}, viewport_scale[4] = {};
    bool lighting = false, specular = false, fog = false, normalization = false;
    uint32_t skin_mode = 0, fog_gen_mode = 0, fog_mode = 0x2601;
    float fog_plane[4] = {}, fog_params[3] = {};

    // Lighting. D3D folds the material into these: the light colours are already light x material.
    float inverse_model_view[4][16] = {};
    float light[8][32] = {};  // NV097_SET_LIGHT_*, 0x80 bytes per light
    uint32_t light_enable_mask = 0, light_control = 0, color_material = 0;
    float scene_ambient[3] = {}, material_emission[3] = {}, material_alpha = 1;
    float specular_params[6] = {};
    float eye_position[4] = {};
};

// Float offsets inside one light's block of NV097_SET_LIGHT_* methods.
enum : int { kLightAmbient = 0, kLightDiffuse = 3, kLightSpecular = 6, kLightRange = 9, kLightHalfVector = 10,
             kLightDirection = 13, kLightSpotFalloff = 16, kLightSpotDirection = 19, kLightPosition = 23,
             kLightAttenuation = 26 };

Kelvin g_k;
std::set<uint32_t> g_logged;
uint64_t g_draws = 0;
int g_debug_draws = -1;  // MC3_DEBUG_DRAWS=<n>: describe the first n draws in the log

void log_once(uint32_t key, const char* message) {
    if (g_logged.insert(key).second) log_line("nv2a: %s", message);
}

float as_float(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

uint32_t dma_address(uint32_t context_dma, uint32_t offset) {
    return (context_dma == 2 ? g_k.dma_b : g_k.dma_a) + offset;
}

SurfaceState surface() {
    SurfaceState s{};
    s.color_address = g_k.dma_color + g_k.color_offset;
    s.zeta_address = (g_k.surface_format & 0xF0) ? g_k.dma_zeta + g_k.zeta_offset : 0;
    s.width = g_k.clip_horizontal >> 16;
    s.height = g_k.clip_vertical >> 16;
    constexpr uint32_t kSurfaceSwizzled = 2;
    if (((g_k.surface_format >> 8) & 0xF) == kSurfaceSwizzled) {
        // A swizzled surface is a whole power-of-two texture (log2 sizes in the format); the clip only limits
        // where this draw writes, and the texture's coordinates are relative to its full size.
        s.width = 1u << ((g_k.surface_format >> 16) & 0xF);
        s.height = 1u << ((g_k.surface_format >> 24) & 0xF);
    }
    return s;
}

// --- vertex fetch ---

uint32_t type_size(uint32_t type) {
    switch (type) {
        case kTypeUbD3d: case kTypeUbOgl: return 1;
        case kTypeS1: case kTypeS32k: return 2;
        case kTypeCompressed: return 4;
        default: return 4;
    }
}

uint32_t attribute_bytes(uint32_t format) {
    const uint32_t size = (format >> 4) & 0xF;
    const uint32_t type = format & 0xF;
    return type == kTypeCompressed ? 4 * size : size * type_size(type);
}

float signed_bits(uint32_t value, uint32_t bits) {
    const int32_t shifted = static_cast<int32_t>(value << (32 - bits)) >> (32 - bits);
    return std::max(-1.0f, static_cast<float>(shifted) / static_cast<float>((1 << (bits - 1)) - 1));
}

Vec4 read_attribute(const uint8_t* p, uint32_t format) {
    const uint32_t size = (format >> 4) & 0xF;
    const uint32_t type = format & 0xF;
    Vec4 v{0, 0, 0, 1};
    switch (type) {
        case kTypeUbD3d:  // D3DCOLOR: B, G, R, A in memory
            if (size >= 4) v = Vec4{p[2] / 255.0f, p[1] / 255.0f, p[0] / 255.0f, p[3] / 255.0f};
            else for (uint32_t i = 0; i < size; ++i) v[i] = p[i] / 255.0f;
            break;
        case kTypeUbOgl:
            for (uint32_t i = 0; i < size && i < 4; ++i) v[i] = p[i] / 255.0f;
            break;
        case kTypeS1:
            for (uint32_t i = 0; i < size && i < 4; ++i) {
                int16_t s;
                std::memcpy(&s, p + i * 2, 2);
                v[i] = std::max(-1.0f, s / 32767.0f);
            }
            break;
        case kTypeS32k:
            for (uint32_t i = 0; i < size && i < 4; ++i) {
                int16_t s;
                std::memcpy(&s, p + i * 2, 2);
                v[i] = s;
            }
            break;
        case kTypeFloat:
            for (uint32_t i = 0; i < size && i < 4; ++i) std::memcpy(&v[i], p + i * 4, 4);
            break;
        case kTypeCompressed: {
            uint32_t packed;
            std::memcpy(&packed, p, 4);
            v = Vec4{signed_bits(packed & 0x7FF, 11), signed_bits((packed >> 11) & 0x7FF, 11),
                     signed_bits(packed >> 22, 10), 1};
            break;
        }
        default: break;
    }
    return v;
}

// --- primitive assembly ---

void append_triangles(uint32_t primitive, uint32_t count, std::vector<uint32_t>& out) {
    switch (primitive) {
        case kTriangles:
            for (uint32_t i = 0; i + 2 < count; i += 3) out.insert(out.end(), {i, i + 1, i + 2});
            break;
        case kTriangleStrip:
            for (uint32_t i = 2; i < count; ++i) {
                if (i & 1) out.insert(out.end(), {i - 1, i - 2, i});
                else out.insert(out.end(), {i - 2, i - 1, i});
            }
            break;
        case kTriangleFan:
        case kPolygon:
            for (uint32_t i = 2; i < count; ++i) out.insert(out.end(), {0u, i - 1, i});
            break;
        case kQuads:
            for (uint32_t i = 0; i + 3 < count; i += 4) out.insert(out.end(), {i, i + 1, i + 2, i, i + 2, i + 3});
            break;
        case kQuadStrip:
            for (uint32_t i = 0; i + 3 < count; i += 2) out.insert(out.end(), {i, i + 1, i + 2, i + 1, i + 3, i + 2});
            break;
        default:
            log_once(0x1000 + primitive, "points and lines are not drawn yet");
            break;
    }
}

// Screen-space position from the vertex program to D3D11 clip space.
RenderVertex to_render_vertex(const Vec4* o, float width, float height) {
    RenderVertex v{};
    float w = o[kOutPosition][3];
    if (!std::isfinite(w) || std::fabs(w) < 5.42101e-20f) w = w < 0 ? -5.42101e-20f : 5.42101e-20f;
    const float x = 2.0f * o[kOutPosition][0] / width - 1.0f;
    const float y = 1.0f - 2.0f * o[kOutPosition][1] / height;
    const float z = g_k.clip_max != 0 ? o[kOutPosition][2] / g_k.clip_max : o[kOutPosition][2];
    v.position[0] = x * w;
    v.position[1] = y * w;
    v.position[2] = z * w;
    v.position[3] = w;
    std::memcpy(v.diffuse, o[kOutDiffuse].data(), 16);
    std::memcpy(v.specular, o[kOutSpecular].data(), 16);
    for (int i = 0; i < 4; ++i) std::memcpy(v.texture[i], o[kOutTexture0 + i].data(), 16);
    v.fog = o[kOutFog][0];
    return v;
}

// Fixed-function vertex attributes (the order the hardware feeds them).
enum : uint32_t { kInPosition = 0, kInWeight = 1, kInNormal = 2, kInDiffuse = 3, kInSpecular = 4, kInFog = 5,
                  kInBackDiffuse = 7, kInBackSpecular = 8, kInTexture0 = 9 };

// NV097_SET_TEXGEN_* modes (the OpenGL values).
constexpr uint32_t kTexgenDisable = 0, kTexgenEyeLinear = 0x2400, kTexgenObjectLinear = 0x2401;

// NV097_SET_FOG_GEN_MODE.
constexpr uint32_t kFogGenSpecularAlpha = 0, kFogGenRadial = 1, kFogGenPlanar = 2, kFogGenAbsPlanar = 3, kFogGenX = 6;

Vec4 transform(const float* matrix, const Vec4& v) {
    Vec4 out;
    for (int row = 0; row < 4; ++row) {
        const float* m = matrix + row * 4;
        out[row] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3] * v[3];
    }
    return out;
}

float dot4(const float* a, const Vec4& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; }

struct Vec3 {
    float x = 0, y = 0, z = 0;
};
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
Vec3 operator*(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length(Vec3 a) { return std::sqrt(dot(a, a)); }
Vec3 normalize(Vec3 a) {
    const float l = length(a);
    return l > 0 ? a * (1.0f / l) : a;
}
Vec3 vec3(const float* p) { return {p[0], p[1], p[2]}; }
Vec3 rgb(const Vec4& v) { return {v[0], v[1], v[2]}; }

// NV097_SET_COLOR_MATERIAL sources (2 bits each: emission, ambient, diffuse, specular).
constexpr uint32_t kSourceMaterial = 0, kSourceDiffuse = 1;
constexpr uint32_t kLightControlSeparateSpecular = 1, kLightControlLocalEye = 1u << 16;
constexpr uint32_t kLightInfinite = 1, kLightLocal = 2, kLightSpot = 3;

// Fixed-function lighting. `eye` is the eye-space position. Writes the front colours (back faces are not lit:
// they keep their vertex colours).
void light_vertex(const Vec4* in, const Vec4& eye, Vec4* out) {
    const Vec4& diffuse = in[kInDiffuse];
    const Vec4& specular = in[kInSpecular];
    const uint32_t emission_source = g_k.color_material & 3, ambient_source = (g_k.color_material >> 2) & 3,
                   diffuse_source = (g_k.color_material >> 4) & 3, specular_source = (g_k.color_material >> 6) & 3;
    const auto source = [&](uint32_t s) { return s == kSourceDiffuse ? rgb(diffuse) : rgb(specular); };
    const Vec3 emission_factor = vec3(g_k.material_emission);

    // Emission and ambient: vertex-coloured terms are scaled by the material register, material ones are folded
    // into the scene ambient.
    Vec3 front;
    if (emission_source == kSourceMaterial || ambient_source == kSourceMaterial) {
        front = vec3(g_k.scene_ambient);
        if (emission_source != kSourceMaterial) front = front + source(emission_source) * emission_factor;
        if (ambient_source != kSourceMaterial) front = front + source(ambient_source) * emission_factor;
    } else {
        front = source(emission_source) + source(ambient_source) * emission_factor;
    }
    const float alpha = diffuse_source == kSourceMaterial ? g_k.material_alpha
                        : diffuse_source == kSourceDiffuse ? diffuse[3] : specular[3];
    Vec3 lit_specular;

    Vec3 normal = rgb(transform(g_k.inverse_model_view[0], {in[kInNormal][0], in[kInNormal][1], in[kInNormal][2], 0}));
    if (g_k.normalization) normal = normalize(normal);
    const float w = eye[3] != 0 ? eye[3] : 1;
    const Vec3 position{eye[0] / w, eye[1] / w, eye[2] / w};
    const bool local_eye = (g_k.light_control & kLightControlLocalEye) != 0;
    const Vec3 to_eye = normalize(vec3(g_k.eye_position) - position);
    const float* sp = g_k.specular_params;

    for (int i = 0; i < 8; ++i) {
        const uint32_t type = (g_k.light_enable_mask >> (i * 2)) & 3;
        if (!type) continue;
        const float* l = g_k.light[i];
        float attenuation = 1;
        Vec3 direction, half;
        if (type == kLightInfinite) {
            direction = normalize(vec3(l + kLightDirection));
            half = local_eye ? normalize(direction + to_eye) : vec3(l + kLightHalfVector);
        } else {
            Vec3 to_light = vec3(l + kLightPosition) - position;
            const float d = length(to_light);
            if (d > l[kLightRange]) continue;
            direction = normalize(to_light);
            const float* a = l + kLightAttenuation;
            const float denominator = a[0] + a[1] * d + a[2] * d * d;
            attenuation = denominator != 0 ? 1 / denominator : 1;
            half = normalize(direction + (local_eye ? to_eye : Vec3{}));
            if (type == kLightSpot) {
                const float* s = l + kLightSpotDirection;
                const float* f = l + kLightSpotFalloff;
                const float cosine = s[0] * direction.x + s[1] * direction.y + s[2] * direction.z + s[3];
                float spot = 1;
                if (cosine <= 0) {
                    spot = 0;
                } else if (cosine < 1 && cosine > -f[0]) {
                    const float denom = f[1] * cosine + f[2];
                    spot = denom != 0 ? std::clamp((cosine + f[0]) / denom, 0.0f, 1.0f) : 1;
                }
                attenuation *= spot;
            }
        }
        const float n_dot_l = std::max(0.0f, dot(normal, direction));
        const float n_dot_h = std::max(0.0f, dot(normal, half));
        float power = 0;
        if (n_dot_l > 0 && n_dot_h > -sp[0]) {
            const float denominator = sp[1] * n_dot_h + sp[2];
            power = denominator == 0 ? 10000.0f
                    : denominator > 0 ? std::clamp((n_dot_h + sp[0]) / denominator, 0.0f, 10000.0f) : 0;
        }
        front = front + vec3(l + kLightAmbient) * attenuation;
        Vec3 diffuse_term = vec3(l + kLightDiffuse) * (attenuation * n_dot_l);
        if (diffuse_source != kSourceMaterial) diffuse_term = diffuse_term * source(diffuse_source);
        front = front + diffuse_term;
        Vec3 specular_term = vec3(l + kLightSpecular) * (attenuation * power);
        if (specular_source != kSourceMaterial) specular_term = specular_term * source(specular_source);
        lit_specular = lit_specular + specular_term;
    }

    out[kOutDiffuse] = {front.x, front.y, front.z, alpha};
    if (!g_k.specular) {
        out[kOutSpecular] = {0, 0, 0, 1};
    } else if (g_k.light_control & kLightControlSeparateSpecular) {
        out[kOutSpecular] = {lit_specular.x, lit_specular.y, lit_specular.z, specular[3]};
    } else {
        out[kOutDiffuse] = {front.x + lit_specular.x, front.y + lit_specular.y, front.z + lit_specular.z, alpha};
        out[kOutSpecular] = specular;
    }
}

// Fog factor from the fog distance (oFog.x of a vertex program, or the fixed-function fog generation):
// 1 = no fog. The pixel shader clamps it to 0..1.
float fog_factor(float distance) {
    if (!g_k.fog) return 1;
    if (std::isinf(distance)) return g_k.fog_mode == 0x801 || g_k.fog_mode == 0x803 ? 0.0f : 1.0f;
    float factor = 1;
    switch (g_k.fog_mode) {
        case 0x2601: case 0x804:  // LINEAR, LINEAR_ABS
            factor = g_k.fog_params[0] + distance * g_k.fog_params[1] - 1.0f;
            break;
        case 0x800: case 0x802:  // EXP, EXP_ABS
            factor = g_k.fog_params[0] + std::exp2(distance * g_k.fog_params[1] * 16.0f) - 1.5f;
            break;
        case 0x801: case 0x803:  // EXP2, EXP2_ABS
            factor = g_k.fog_params[0] +
                     std::exp2(-distance * distance * g_k.fog_params[1] * g_k.fog_params[1] * 32.0f) - 1.5f;
            break;
        default: log_once(0x2300 + g_k.fog_mode, "fog mode not supported yet"); break;
    }
    if (g_k.fog_mode == 0x802 || g_k.fog_mode == 0x803 || g_k.fog_mode == 0x804) factor = std::fabs(factor);
    return std::isnan(factor) ? 1.0f : factor;
}

// The fixed-function transform, producing the same outputs a vertex program would: a screen-space position
// (x and y divided by w, the viewport offset added), colours, texture coordinates and fog.
void transform_fixed_function(const Vec4* in, Vec4* out) {
    if (g_k.skin_mode) log_once(0x2001, "fixed-function skinning is not supported yet; the first matrix is used");
    const Vec4& position = in[kInPosition];
    const Vec4 eye = transform(g_k.model_view[0], position);  // what eye-space texgen and fog see

    Vec4 p = transform(g_k.composite, position);  // model view, projection and viewport scale in one
    float w = p[3];
    if (!std::isfinite(w) || std::fabs(w) < 5.42101e-20f) w = w < 0 ? -5.42101e-20f : 5.42101e-20f;
    out[kOutPosition] = {p[0] / w + g_k.viewport_offset[0], p[1] / w + g_k.viewport_offset[1], p[2] / w, w};

    out[kOutBackDiffuse] = in[kInBackDiffuse];
    out[kOutBackSpecular] = g_k.specular ? in[kInBackSpecular] : Vec4{0, 0, 0, 1};
    if (g_k.lighting) {
        light_vertex(in, eye, out);
    } else {
        out[kOutDiffuse] = in[kInDiffuse];
        out[kOutSpecular] = g_k.specular ? in[kInSpecular] : Vec4{0, 0, 0, 1};
    }

    for (int stage = 0; stage < 4; ++stage) {
        Vec4 t = in[kInTexture0 + stage];
        for (int c = 0; c < 4; ++c) {
            switch (g_k.texgen[stage][c]) {
                case kTexgenDisable: break;
                case kTexgenEyeLinear: t[c] = dot4(g_k.texgen_plane[stage][c], eye); break;
                case kTexgenObjectLinear: t[c] = dot4(g_k.texgen_plane[stage][c], position); break;
                default:
                    log_once(0x2100 + g_k.texgen[stage][c], "fixed-function texgen mode not supported yet");
                    break;
            }
        }
        out[kOutTexture0 + stage] = g_k.texture_matrix_enable[stage] ? transform(g_k.texture_matrix[stage], t) : t;
    }

    float fog = 0;
    if (g_k.fog) {
        const float planar = g_k.fog_plane[0] * eye[0] + g_k.fog_plane[1] * eye[1] + g_k.fog_plane[2] * eye[2] +
                             g_k.fog_plane[3];
        switch (g_k.fog_gen_mode) {
            case kFogGenSpecularAlpha: fog = std::clamp(in[kInSpecular][3], 0.0f, 1.0f); break;
            case kFogGenRadial: fog = std::sqrt(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]); break;
            case kFogGenPlanar: fog = planar; break;
            case kFogGenAbsPlanar: fog = std::fabs(planar); break;
            case kFogGenX: fog = in[kInFog][0]; break;
            default: log_once(0x2200 + g_k.fog_gen_mode, "fog generation mode not supported yet"); break;
        }
    }
    out[kOutFog] = {fog, fog, fog, fog};
}

void prepare_draw_state() {
    DrawState& d = g_k.draw;
    for (int i = 0; i < 8; ++i) {
        unpack_color(g_k.combiner_factor0[i], d.constants.factor0[i]);
        unpack_color(g_k.combiner_factor1[i], d.constants.factor1[i]);
    }
    std::memcpy(d.constants.final_factor0, d.constants.factor0[0], 16);
    std::memcpy(d.constants.final_factor1, d.constants.factor1[0], 16);
    d.constants.alpha_reference[0] = g_k.alpha_reference / 255.0f;
    d.program.alpha_test = g_k.alpha_test ? g_k.alpha_function : 0;
    for (int i = 0; i < 4; ++i) {
        TextureStage& stage = d.textures[i];
        stage.enabled = (g_k.texture_control0[i] & 0x40000000) != 0;
        stage.descriptor.address = dma_address(g_k.texture_format[i] & 3, g_k.texture_offset[i]);
        stage.descriptor.format = g_k.texture_format[i];
        stage.descriptor.pitch = g_k.texture_control1[i] >> 16;
        stage.descriptor.rect = g_k.texture_rect[i];
        stage.descriptor.palette = dma_address((g_k.texture_palette[i] & 1) ? 2 : 1, g_k.texture_palette[i] & ~0x3Fu);
        float* scale = d.constants.texture_scale[i];
        scale[0] = scale[1] = scale[2] = scale[3] = 1.0f;
        const uint32_t width = stage.descriptor.rect >> 16, height = stage.descriptor.rect & 0xFFFF;
        if (texture_is_linear(stage.descriptor) && width && height) {
            scale[0] = 1.0f / width;
            scale[1] = 1.0f / height;
        }
    }
}

// Runs the vertex program over `count` vertices obtained from `fetch` and draws the primitive.
template <typename Fetch>
void draw(uint32_t count, Fetch fetch) {
    if (count == 0 || g_k.primitive == kEnd) return;
    const bool fixed_function = (g_k.execution_mode & 3) != kExecutionModeProgram;
    if (!fixed_function) {
        if (g_k.program_dirty) {
            g_k.vertex_program.decode(g_k.program, g_k.program_start);
            g_k.program_dirty = false;
        }
        if (g_k.vertex_program.empty()) return;
    }
    const SurfaceState target = surface();
    if (!target.width || !target.height) return;

    std::vector<RenderVertex> vertices(count);
    Vec4 inputs[kVshAttributes];
    Vec4 outputs[kVshOutputs];
    LARGE_INTEGER vertex_start, vertex_end;
    QueryPerformanceCounter(&vertex_start);
    for (uint32_t i = 0; i < count; ++i) {
        std::copy(std::begin(g_k.attribute), std::end(g_k.attribute), inputs);
        fetch(i, inputs);
        if (fixed_function) {
            transform_fixed_function(inputs, outputs);
        } else {
            g_k.vertex_program.execute(inputs, g_k.constants, outputs);
        }
        outputs[kOutFog][0] = fog_factor(outputs[kOutFog][0]);
        vertices[i] = to_render_vertex(outputs, static_cast<float>(target.width), static_cast<float>(target.height));
    }
    QueryPerformanceCounter(&vertex_end);
    renderer_note_vertex_work(static_cast<uint64_t>(vertex_end.QuadPart - vertex_start.QuadPart), count);
    static bool fixed_function_only = false;  // MC3_DEBUG_DRAWS_FF=1: describe only fixed-function draws
    if (g_debug_draws < 0) {
        char value[16] = {};
        g_debug_draws = GetEnvironmentVariableA("MC3_DEBUG_DRAWS", value, sizeof(value)) ? std::atoi(value) : 0;
        fixed_function_only = GetEnvironmentVariableA("MC3_DEBUG_DRAWS_FF", value, sizeof(value)) != 0;
    }
    if (g_debug_draws > 0 && (fixed_function || !fixed_function_only)) {
        --g_debug_draws;
        log_line("draw %llu: primitive %u, %u vertices, surface %08X %ux%u, clip max %g", static_cast<unsigned long long>(g_draws),
                 g_k.primitive, count, target.color_address, target.width, target.height, g_k.clip_max);
        for (uint32_t i = 0; i < count && i < 3; ++i) {
            const RenderVertex& v = vertices[i];
            log_line("  v%u: clip (%g %g %g %g) d0 (%.2f %.2f %.2f %.2f) t0 (%g %g %g %g)", i, v.position[0],
                     v.position[1], v.position[2], v.position[3], v.diffuse[0], v.diffuse[1], v.diffuse[2], v.diffuse[3],
                     v.texture[0][0], v.texture[0][1], v.texture[0][2], v.texture[0][3]);
        }
        if (fixed_function) {
            const float* c = g_k.composite;
            log_line("  fixed function: lighting %d specular %d skin %u fog %d, viewport offset (%g %g %g %g)",
                     g_k.lighting, g_k.specular, g_k.skin_mode, g_k.fog, g_k.viewport_offset[0],
                     g_k.viewport_offset[1], g_k.viewport_offset[2], g_k.viewport_offset[3]);
            log_line("  composite (%g %g %g %g) (%g %g %g %g) (%g %g %g %g) (%g %g %g %g)", c[0], c[1], c[2], c[3], c[4],
                     c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15]);
            const Vec4 first = [&] {
                Vec4 in[kVshAttributes];
                std::copy(std::begin(g_k.attribute), std::end(g_k.attribute), in);
                fetch(0, in);
                return in[0];
            }();
            log_line("  v0 input position (%g %g %g %g)", first[0], first[1], first[2], first[3]);
        }
        const CombinerProgram& p = g_k.draw.program;
        log_line("  combiners: control %08X stages %08X final %08X %08X color in0 %08X out0 %08X alpha in0 %08X out0 %08X",
                 p.control, p.stage_program, p.final0, p.final1, p.color_input[0], p.color_output[0], p.alpha_input[0],
                 p.alpha_output[0]);
        for (int t = 0; t < 4; ++t) {
            if (g_k.texture_control0[t] & 0x40000000) {
                const uint32_t address = dma_address(g_k.texture_format[t] & 3, g_k.texture_offset[t]);
                uint32_t max_byte = 0;
                uint64_t sum = 0;
                if (const uint8_t* texels = physical_pointer(address)) {
                    for (uint32_t b = 0; b < 640 * 480 * 4; b += 4 * 97) {
                        max_byte = std::max<uint32_t>(max_byte, texels[b]);
                        sum += texels[b];
                    }
                }
                log_line("  texture %d: offset %08X address %08X (dma a %08X b %08X) format %08X rect %08X pitch %u, blue max %u sum %llu",
                         t, g_k.texture_offset[t], address, g_k.dma_a, g_k.dma_b, g_k.texture_format[t], g_k.texture_rect[t],
                         g_k.texture_control1[t] >> 16, max_byte, static_cast<unsigned long long>(sum));
            }
        }
        log_line("  blend %d (%X %X) depth %d write %d alpha test %d mask %08X", g_k.draw.blend_enable,
                 g_k.draw.blend_source, g_k.draw.blend_destination, g_k.draw.depth_test, g_k.draw.depth_write,
                 g_k.alpha_test, g_k.draw.color_mask);
    }
    std::vector<uint32_t> order;
    append_triangles(g_k.primitive, count, order);
    std::vector<RenderVertex> triangles;
    triangles.reserve(order.size());
    for (uint32_t index : order) triangles.push_back(vertices[index]);
    prepare_draw_state();
    renderer_draw(target, g_k.draw, triangles);
    ++g_draws;

    // MC3_DEBUG_FRAME_AT=<seconds>: one line per draw during 1 s from that time (counted from the first draw), to
    // see what the frames are made of.
    static ULONGLONG window_start = 0, window_end = 0;
    if (!window_start) {
        char value[16] = {};
        const ULONGLONG now = GetTickCount64();
        window_start = GetEnvironmentVariableA("MC3_DEBUG_FRAME_AT", value, sizeof(value))
                           ? now + static_cast<ULONGLONG>(std::atof(value) * 1000)
                           : ~0ull;
        window_end = window_start + 1000;
    }
    const ULONGLONG now = GetTickCount64();
    if (now >= window_start && now < window_end) {
        const DrawState& d = g_k.draw;
        const RenderVertex& v = vertices[0];
        log_line("frame draw %llu: %s prim %u n %u surf %08X | tex0 %s %08X fmt %08X | blend %d %X %X | z %d %X w %d | "
                 "atest %d %X ref %02X | mask %08X | clip (%.3g %.3g %.3g %.3g) d0 (%.2f %.2f %.2f %.2f) | comb %08X %08X %08X",
                 static_cast<unsigned long long>(g_draws), fixed_function ? "ff" : "vp", g_k.primitive, count,
                 target.color_address, d.textures[0].enabled ? "on" : "off", d.textures[0].descriptor.address,
                 d.textures[0].descriptor.format, d.blend_enable, d.blend_source, d.blend_destination, d.depth_test,
                 d.depth_function, d.depth_write, g_k.alpha_test, g_k.alpha_function, g_k.alpha_reference,
                 d.color_mask, v.position[0] / v.position[3], v.position[1] / v.position[3],
                 v.position[2] / v.position[3], v.position[3], v.diffuse[0], v.diffuse[1], v.diffuse[2],
                 v.diffuse[3], d.program.color_input[0], d.program.final0, d.program.final1);
    }
}

void draw_inline() {
    uint32_t offsets[kVshAttributes];
    uint32_t stride = 0;
    for (uint32_t i = 0; i < kVshAttributes; ++i) {
        offsets[i] = stride;
        if ((g_k.array_format[i] >> 4) & 0xF) stride += (attribute_bytes(g_k.array_format[i]) + 3) & ~3u;
    }
    if (stride == 0) return;
    const auto* bytes = reinterpret_cast<const uint8_t*>(g_k.inline_data.data());
    const uint32_t count = static_cast<uint32_t>(g_k.inline_data.size() * 4 / stride);
    draw(count, [&](uint32_t vertex, Vec4* inputs) {
        for (uint32_t a = 0; a < kVshAttributes; ++a) {
            if ((g_k.array_format[a] >> 4) & 0xF) inputs[a] = read_attribute(bytes + vertex * stride + offsets[a], g_k.array_format[a]);
        }
    });
}

void draw_indexed(const std::vector<uint32_t>& indices) {
    draw(static_cast<uint32_t>(indices.size()), [&](uint32_t vertex, Vec4* inputs) {
        for (uint32_t a = 0; a < kVshAttributes; ++a) {
            const uint32_t format = g_k.array_format[a];
            if (!((format >> 4) & 0xF)) continue;
            const uint32_t offset = g_k.array_offset[a];
            const uint32_t base = (offset & 0x80000000) ? g_k.dma_vertex_b : g_k.dma_vertex_a;
            const uint32_t address = base + (offset & 0x7FFFFFFF) + indices[vertex] * (format >> 8);
            if (const uint8_t* p = physical_pointer(address)) inputs[a] = read_attribute(p, format);
        }
    });
}

void end_primitive() {
    if (!g_k.inline_data.empty()) {
        draw_inline();
    } else if (!g_k.elements.empty()) {
        draw_indexed(g_k.elements);
    }
    g_k.inline_data.clear();
    g_k.elements.clear();
    g_k.primitive = kEnd;
}

}  // namespace

void kelvin_initialise(DmaResolver resolver) {
    g_k.dma = resolver;
    g_k.draw.color_mask = 0x01010101;
    g_k.draw.depth_function = 0x201;
    for (Vec4& attribute : g_k.attribute) attribute = Vec4{0, 0, 0, 1};
}

bool kelvin_method(uint32_t method, uint32_t argument) {
    DrawState& d = g_k.draw;
    CombinerProgram& p = d.program;

    // Arrays of methods.
    if (method >= 0x0B00 && method < 0x0B80) {  // SET_TRANSFORM_PROGRAM
        const uint32_t slot = (method - 0x0B00) / 4;
        if (g_k.program_load < kVshProgramSlots) g_k.program[g_k.program_load * 4 + slot % 4] = argument;
        if (slot % 4 == 3) ++g_k.program_load;
        g_k.program_dirty = true;
        return true;
    }
    if (method >= 0x0B80 && method < 0x0C00) {  // SET_TRANSFORM_CONSTANT
        const uint32_t slot = (method - 0x0B80) / 4;
        if (g_k.constant_load < kVshConstants) g_k.constants[g_k.constant_load][slot % 4] = as_float(argument);
        if (slot % 4 == 3) ++g_k.constant_load;
        return true;
    }
    if (method >= 0x1720 && method < 0x1760) { g_k.array_offset[(method - 0x1720) / 4] = argument; return true; }
    if (method >= 0x1760 && method < 0x17A0) { g_k.array_format[(method - 0x1760) / 4] = argument; return true; }
    if (method >= 0x1940 && method < 0x1980) {  // SET_VERTEX_DATA4UB
        Vec4& v = g_k.attribute[(method - 0x1940) / 4];
        for (int i = 0; i < 4; ++i) v[i] = ((argument >> (i * 8)) & 0xFF) / 255.0f;
        return true;
    }
    if (method >= 0x1A00 && method < 0x1B00) {  // SET_VERTEX_DATA4F_M
        g_k.attribute[(method - 0x1A00) / 16][((method - 0x1A00) / 4) % 4] = as_float(argument);
        return true;
    }
    if (method >= 0x1880 && method < 0x1900) {  // SET_VERTEX_DATA2F_M
        Vec4& v = g_k.attribute[(method - 0x1880) / 8];
        v[((method - 0x1880) / 4) % 2] = as_float(argument);
        v[2] = 0;
        v[3] = 1;
        return true;
    }
    // Fixed-function transform state.
    if (method >= 0x0480 && method < 0x0580) {  // SET_MODEL_VIEW_MATRIX0..3
        g_k.model_view[(method - 0x0480) / 0x40][((method - 0x0480) % 0x40) / 4] = as_float(argument);
        return true;
    }
    if (method >= 0x0680 && method < 0x06C0) { g_k.composite[(method - 0x0680) / 4] = as_float(argument); return true; }
    if (method >= 0x06C0 && method < 0x07C0) {  // SET_TEXTURE_MATRIX0..3
        g_k.texture_matrix[(method - 0x06C0) / 0x40][((method - 0x06C0) % 0x40) / 4] = as_float(argument);
        return true;
    }
    if (method >= 0x0420 && method < 0x0430) { g_k.texture_matrix_enable[(method - 0x0420) / 4] = argument != 0; return true; }
    if (method >= 0x03C0 && method < 0x0400) {  // SET_TEXGEN_S/T/R/Q, 0x10 per stage
        g_k.texgen[(method - 0x03C0) / 0x10][((method - 0x03C0) % 0x10) / 4] = argument;
        return true;
    }
    if (method >= 0x0840 && method < 0x0940) {  // SET_TEXGEN_PLANE_S/T/R/Q, 0x40 per stage
        const uint32_t offset = method - 0x0840;
        g_k.texgen_plane[offset / 0x40][(offset % 0x40) / 0x10][(offset % 0x10) / 4] = as_float(argument);
        return true;
    }
    if (method >= 0x0A20 && method < 0x0A30) { g_k.viewport_offset[(method - 0x0A20) / 4] = as_float(argument); return true; }
    if (method >= 0x0AF0 && method < 0x0B00) { g_k.viewport_scale[(method - 0x0AF0) / 4] = as_float(argument); return true; }
    if (method >= 0x09D0 && method < 0x09E0) { g_k.fog_plane[(method - 0x09D0) / 4] = as_float(argument); return true; }
    if (method >= 0x09C0 && method < 0x09CC) { g_k.fog_params[(method - 0x09C0) / 4] = as_float(argument); return true; }

    // Lighting state.
    if (method >= 0x0580 && method < 0x0680) {  // SET_INVERSE_MODEL_VIEW_MATRIX0..3
        g_k.inverse_model_view[(method - 0x0580) / 0x40][((method - 0x0580) % 0x40) / 4] = as_float(argument);
        return true;
    }
    if (method >= 0x1000 && method < 0x1400) {  // SET_LIGHT_*, 0x80 per light
        g_k.light[(method - 0x1000) / 0x80][((method - 0x1000) % 0x80) / 4] = as_float(argument);
        return true;
    }
    if (method >= 0x0C00 && method < 0x0E00) return true;  // SET_BACK_LIGHT_*: two-sided lighting, not yet
    if (method >= 0x09E0 && method < 0x09F8) { g_k.specular_params[(method - 0x09E0) / 4] = as_float(argument); return true; }
    if (method >= 0x1E28 && method < 0x1E40) return true;  // SET_SPECULAR_PARAMS_BACK
    if (method >= 0x0A10 && method < 0x0A1C) { g_k.scene_ambient[(method - 0x0A10) / 4] = as_float(argument); return true; }
    if (method >= 0x03A8 && method < 0x03B4) { g_k.material_emission[(method - 0x03A8) / 4] = as_float(argument); return true; }
    if (method >= 0x0A50 && method < 0x0A60) { g_k.eye_position[(method - 0x0A50) / 4] = as_float(argument); return true; }

    if (method >= 0x0260 && method < 0x0280) { p.alpha_input[(method - 0x0260) / 4] = argument; return true; }
    if (method >= 0x0A60 && method < 0x0A80) { g_k.combiner_factor0[(method - 0x0A60) / 4] = argument; return true; }
    if (method >= 0x0A80 && method < 0x0AA0) { g_k.combiner_factor1[(method - 0x0A80) / 4] = argument; return true; }
    if (method >= 0x0AA0 && method < 0x0AC0) { p.alpha_output[(method - 0x0AA0) / 4] = argument; return true; }
    if (method >= 0x0AC0 && method < 0x0AE0) { p.color_input[(method - 0x0AC0) / 4] = argument; return true; }
    if (method >= 0x1E40 && method < 0x1E60) { p.color_output[(method - 0x1E40) / 4] = argument; return true; }
    if (method >= 0x1B00 && method < 0x1C00) {  // SET_TEXTURE_*, 0x40 per stage
        const uint32_t stage = (method - 0x1B00) / 0x40;
        switch ((method - 0x1B00) % 0x40) {
            case 0x00: g_k.texture_offset[stage] = argument; return true;
            case 0x04: g_k.texture_format[stage] = argument; return true;
            case 0x08: d.textures[stage].address_mode = argument; return true;
            case 0x0C: g_k.texture_control0[stage] = argument; return true;
            case 0x10: g_k.texture_control1[stage] = argument; return true;
            case 0x14: d.textures[stage].filter = argument; return true;
            case 0x1C: g_k.texture_rect[stage] = argument; return true;
            case 0x20: g_k.texture_palette[stage] = argument; return true;
            default: return true;  // border colour, bump-map matrices: not used yet
        }
    }

    switch (method) {
        case 0x0184: g_k.dma_a = g_k.dma(argument); return true;
        case 0x0188: g_k.dma_b = g_k.dma(argument); return true;
        case 0x0194: g_k.dma_color = g_k.dma(argument); return true;
        case 0x0198: g_k.dma_zeta = g_k.dma(argument); return true;
        case 0x019C: g_k.dma_vertex_a = g_k.dma(argument); return true;
        case 0x01A0: g_k.dma_vertex_b = g_k.dma(argument); return true;
        case 0x0200: g_k.clip_horizontal = argument; return true;
        case 0x0204: g_k.clip_vertical = argument; return true;
        case 0x0208:
            if (g_k.surface_format != argument) {
                log_line("nv2a: surface format %08X (colour %X, zeta %X, type %X, anti-aliasing %X)", argument,
                         argument & 0xF, (argument >> 4) & 0xF, (argument >> 8) & 0xF, (argument >> 12) & 0xF);
            }
            g_k.surface_format = argument;
            return true;
        case 0x020C: g_k.surface_pitch = argument; return true;
        case 0x0210: g_k.color_offset = argument; return true;
        case 0x0214: g_k.zeta_offset = argument; return true;
        case 0x02A8:  // SET_FOG_COLOR: red in the low byte (ABGR), unlike the ARGB colours elsewhere
            for (int i = 0; i < 4; ++i) d.constants.fog_color[i] = ((argument >> (i * 8)) & 0xFF) / 255.0f;
            return true;
        case 0x0294: g_k.light_control = argument; return true;
        case 0x0298: g_k.color_material = argument; return true;
        case 0x029C: g_k.fog_mode = argument; return true;
        case 0x03A4: g_k.normalization = argument != 0; return true;
        case 0x03B4: g_k.material_alpha = as_float(argument); return true;
        case 0x03BC: g_k.light_enable_mask = argument; return true;
        case 0x02A0: g_k.fog_gen_mode = argument; return true;
        case 0x02A4: g_k.fog = argument != 0; return true;
        case 0x0314: g_k.lighting = argument != 0; return true;
        case 0x0328: g_k.skin_mode = argument; return true;
        case 0x03B8: g_k.specular = argument != 0; return true;
        case 0x0288: p.final0 = argument; return true;
        case 0x028C: p.final1 = argument; return true;
        case 0x0300: g_k.alpha_test = argument != 0; return true;
        case 0x0304: d.blend_enable = argument != 0; return true;
        case 0x030C: d.depth_test = argument != 0; return true;
        case 0x033C: g_k.alpha_function = argument; return true;
        case 0x0340: g_k.alpha_reference = argument; return true;
        case 0x0344: d.blend_source = argument; return true;
        case 0x0348: d.blend_destination = argument; return true;
        case 0x034C: unpack_color(argument, d.blend_color); return true;
        case 0x0350: d.blend_equation = argument; return true;
        case 0x0354: d.depth_function = argument; return true;
        case 0x0358: d.color_mask = argument; return true;
        case 0x035C: d.depth_write = argument != 0; return true;
        case 0x0394: g_k.clip_min = as_float(argument); return true;
        case 0x0398: g_k.clip_max = as_float(argument); return true;
        case 0x1E60: p.control = argument; return true;
        case 0x1E70: p.stage_program = argument; return true;
        case 0x1E94: g_k.execution_mode = argument; return true;
        case 0x1E9C: g_k.program_load = argument; return true;
        case 0x1EA0:
            g_k.program_start = argument;
            g_k.program_dirty = true;
            return true;
        case 0x1EA4: g_k.constant_load = argument; return true;
        case 0x17FC:  // SET_BEGIN_END
            if (argument == kEnd) {
                end_primitive();
            } else {
                g_k.primitive = argument;
                g_k.inline_data.clear();
                g_k.elements.clear();
            }
            return true;
        case 0x1818: g_k.inline_data.push_back(argument); return true;  // INLINE_ARRAY
        case 0x1800:                                                     // ARRAY_ELEMENT16
            g_k.elements.push_back(argument & 0xFFFF);
            g_k.elements.push_back(argument >> 16);
            return true;
        case 0x1808: g_k.elements.push_back(argument); return true;  // ARRAY_ELEMENT32
        case 0x1810: {                                               // DRAW_ARRAYS
            const uint32_t start = argument & 0xFFFFFF;
            const uint32_t count = (argument >> 24) + 1;
            for (uint32_t i = 0; i < count; ++i) g_k.elements.push_back(start + i);
            return true;
        }
        case 0x1D8C: g_k.zstencil_clear = argument; return true;
        case 0x1D90: g_k.color_clear = argument; return true;
        case 0x1D98: g_k.clear_horizontal = argument; return true;
        case 0x1D9C: g_k.clear_vertical = argument; return true;
        case 0x1D94:  // CLEAR_SURFACE
            renderer_clear(surface(), argument, g_k.color_clear, g_k.zstencil_clear, g_k.clear_horizontal & 0xFFFF,
                           g_k.clear_vertical & 0xFFFF, g_k.clear_horizontal >> 16, g_k.clear_vertical >> 16);
            return true;
        default:
            return false;
    }
}

}  // namespace mc3::pc
