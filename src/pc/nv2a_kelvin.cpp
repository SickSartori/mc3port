#include "pc/nv2a_kelvin.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <windows.h>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#include "pc/log.h"
#include "pc/nv2a_combiner.h"
#include "pc/nv2a_texture.h"
#include "pc/nv2a_vsh.h"
#include "pc/physical_memory.h"
#include "pc/renderer_d3d11.h"
#include "pc/settings.h"
#include "pc/xbox_thread.h"

namespace mc3::pc {
namespace {

// Primitive types (NV097_SET_BEGIN_END).
enum : uint32_t { kEnd = 0, kPoints, kLines, kLineLoop, kLineStrip, kTriangles, kTriangleStrip, kTriangleFan, kQuads,
                  kQuadStrip, kPolygon };

// Vertex attribute types (NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE).
enum : uint32_t { kTypeUbD3d = 0, kTypeS1 = 1, kTypeFloat = 2, kTypeUbOgl = 4, kTypeS32k = 5, kTypeCompressed = 6 };

constexpr uint32_t kExecutionModeProgram = 2;

// Slots of the transform constant memory that fixed-function methods also write (NV_IGRAPH_XF_XFCTX_*).
constexpr uint32_t kXfComposite = 0x00;  // 4 rows
constexpr uint32_t kXfEyePosition = 0x38;
constexpr uint32_t kXfViewportScale = 0x3A;   // c[-38] in D3D's vertex shaders
constexpr uint32_t kXfViewportOffset = 0x3B;  // c[-37]

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
bool g_debug_window_open = false;  // inside the MC3_DEBUG_FRAME_AT window (as of the previous draw)
ULONGLONG g_window_start = 0, g_window_end = 0;
bool g_draw_dumped = false;  // MC3_DUMP_DRAW done
int g_probe_x = -1, g_probe_y = -1;
uint32_t g_last_probe[2] = {0xFFFFFFFF, 0xFFFFFFFF};

void log_once(uint32_t key, const char* message) {
    static std::mutex mutex;  // vertex work runs on several threads
    std::lock_guard<std::mutex> lock(mutex);
    if (g_logged.insert(key).second) log_line("nv2a: %s", message);
}

// Helper threads for the vertex work of large draws: vertices are independent, and their transformation on the CPU
// was most of the GPU thread's time (half a second per second in split screen). The GPU thread takes part; the
// helpers run on the emulation cores, away from the game's.
class VertexWorkers {
public:
    // Calls work(first, end) over [0, count) in chunks, on the helpers and the calling thread; returns when done.
    void run(uint32_t count, const std::function<void(uint32_t, uint32_t)>& work) {
        start();
        if (threads_.empty()) {
            work(0, count);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            work_ = &work;
            count_ = count;
            next_.store(0);
            busy_ = static_cast<uint32_t>(threads_.size());
            ++generation_;
        }
        wake_.notify_all();
        take_chunks();
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [&] { return busy_ == 0; });
        work_ = nullptr;
    }

private:
    static constexpr uint32_t kChunk = 128;

    void start() {
        if (started_) return;
        started_ = true;
        const unsigned long cores = emulation_cores();
        uint32_t available = 0;
        for (unsigned long mask = cores; mask; mask &= mask - 1) ++available;
        if (!cores) available = std::max(1u, std::thread::hardware_concurrency()) - 1;
        const uint32_t helpers = std::min<uint32_t>(available > 1 ? available - 1 : 0, 7);  // one is the GPU thread's
        for (uint32_t i = 0; i < helpers; ++i) {
            threads_.emplace_back([this, cores] {
                if (cores) SetThreadAffinityMask(GetCurrentThread(), cores);
                uint64_t seen = 0;
                for (;;) {
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        wake_.wait(lock, [&] { return generation_ != seen; });
                        seen = generation_;
                    }
                    take_chunks();
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (--busy_ == 0) done_.notify_one();
                }
            });
            threads_.back().detach();
        }
        log_line("nv2a: %zu helper threads for vertex work", threads_.size());
    }

    void take_chunks() {
        for (;;) {
            const uint32_t first = next_.fetch_add(kChunk);
            if (first >= count_) return;
            (*work_)(first, std::min(count_, first + kChunk));
        }
    }

    bool started_ = false;
    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable wake_, done_;
    const std::function<void(uint32_t, uint32_t)>* work_ = nullptr;
    uint32_t count_ = 0;
    std::atomic<uint32_t> next_{0};
    uint32_t busy_ = 0;
    uint64_t generation_ = 0;
};

VertexWorkers g_vertex_workers;

float as_float(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

uint32_t dma_address(uint32_t context_dma, uint32_t offset) {
    return (context_dma == 2 ? g_k.dma_b : g_k.dma_a) + offset;
}

std::unordered_map<uint32_t, uint32_t> g_drawn_widths;  // kelvin_drawn_width, by physical address

SurfaceState surface() {
    SurfaceState s{};
    s.color_address = g_k.dma_color + g_k.color_offset;
    s.zeta_address = (g_k.surface_format & 0xF0) ? g_k.dma_zeta + g_k.zeta_offset : 0;
    // The clip only limits where draws write (the HUD's radar shrinks it to its own square); pixel coordinates
    // are relative to the whole surface.
    s.clip_x = g_k.clip_horizontal & 0xFFFF;
    s.clip_width = g_k.clip_horizontal >> 16;
    s.clip_y = g_k.clip_vertical & 0xFFFF;
    s.clip_height = g_k.clip_vertical >> 16;
    constexpr uint32_t kSurfaceSwizzled = 2;
    if (((g_k.surface_format >> 8) & 0xF) == kSurfaceSwizzled) {
        // A swizzled surface is a whole power-of-two texture (log2 sizes in the format).
        s.width = 1u << ((g_k.surface_format >> 16) & 0xF);
        s.height = 1u << ((g_k.surface_format >> 24) & 0xF);
    } else {
        // A linear one is as wide as its pitch; its height is not stated, the clip's bottom is the best bound.
        const uint32_t color_format = g_k.surface_format & 0xF;
        const uint32_t bytes_per_pixel = color_format <= 3 || color_format == 0xA ? 2 : color_format == 9 ? 1 : 4;
        const uint32_t pitch = g_k.surface_pitch & 0xFFFF;
        s.width = std::max(pitch / bytes_per_pixel, s.clip_x + s.clip_width);
        s.height = s.clip_y + s.clip_height;
        uint32_t& drawn = g_drawn_widths[s.color_address & 0x0FFFFFFF];
        drawn = std::max(drawn, s.clip_x + s.clip_width);
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

// Screen-space position from the vertex program (pixels) to the renderer's form: pixels times w, depth 0..1.
RenderVertex to_render_vertex(const Vec4* o) {
    RenderVertex v{};
    float w = o[kOutPosition][3];
    if (!std::isfinite(w) || std::fabs(w) < 5.42101e-20f) w = w < 0 ? -5.42101e-20f : 5.42101e-20f;
    const float x = o[kOutPosition][0];
    const float y = o[kOutPosition][1];
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

// MC3_DEBUG_FRAME_AT=<seconds>[+<length>]: one line per draw during 1 s (or <length> s) from that time (counted
// from the first draw), to see what the frames are made of.
// (Globals, not statics: draw is a template, and each instantiation would get its own window.)
void update_debug_window() {
    const ULONGLONG now = GetTickCount64();
    if (!g_window_start) {
        char value[32] = {};
        const bool set = GetEnvironmentVariableA("MC3_DEBUG_FRAME_AT", value, sizeof(value)) != 0;
        g_window_start = set ? now + static_cast<ULONGLONG>(std::atof(value) * 1000) : ~0ull;
        const char* plus = std::strchr(value, '+');
        g_window_end = g_window_start + static_cast<ULONGLONG>((plus ? std::atof(plus + 1) : 1.0) * 1000);
    }
    g_debug_window_open = now >= g_window_start && now < g_window_end;
}

// DEBUG: vertex time per second by kind of draw (0 fixed function on the CPU, 1 vertex program on the CPU, 2 fixed
// function and 3 vertex program on the GPU: there only the CPU's preparation).
void debug_vertex_work(int kind, uint64_t elapsed, uint32_t count) {
    static uint64_t ticks[4] = {}, vertices[4] = {}, draws[4] = {};
    static ULONGLONG last = GetTickCount64();
    ticks[kind] += elapsed;
    vertices[kind] += count;
    ++draws[kind];
    if (GetTickCount64() - last < 10000) return;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const double seconds = (GetTickCount64() - last) / 1000.0;
    const auto ms = [&](int k) { return ticks[k] * 1000.0 / frequency.QuadPart / seconds; };
    log_line("DEBUG vertex work per second: fixed-function %.0f ms (%.0f draws, %.0f vertices), program %.0f ms "
             "(%.0f draws, %.0f vertices), on the GPU: fixed-function %.0f ms (%.0f draws, %.0f vertices), program "
             "%.0f ms (%.0f draws, %.0f vertices)",
             ms(0), draws[0] / seconds, vertices[0] / seconds, ms(1), draws[1] / seconds, vertices[1] / seconds, ms(2),
             draws[2] / seconds, vertices[2] / seconds, ms(3), draws[3] / seconds, vertices[3] / seconds);
    for (int k = 0; k < 4; ++k) ticks[k] = vertices[k] = draws[k] = 0;
    last = GetTickCount64();
}

// Setting gpu_vertices: the fixed-function transform in a vertex shader instead of on the CPU. The debugging
// switches that look at or filter the transformed vertices (MC3_DEBUG_DRAWS, MC3_SKIP, MC3_DUMP_DRAW) keep the CPU.
bool gpu_fixed_function() {
    static const bool enabled = [] {
        char value[64];
        const bool debugging = GetEnvironmentVariableA("MC3_DEBUG_DRAWS", value, sizeof(value)) ||
                               GetEnvironmentVariableA("MC3_SKIP", value, sizeof(value)) ||
                               GetEnvironmentVariableA("MC3_DUMP_DRAW", value, sizeof(value));
        const bool on = setting("gpu_vertices") != 0 && !debugging && renderer_has_fixed_function();
        log_line("vertices: fixed-function transform on the %s", on ? "GPU" : "CPU");
        return on;
    }();
    return enabled;
}

// The fixed-function attributes the vertex shader reads (position, normal, colours, fog, texture coordinates).
constexpr uint32_t kGpuAttributes = 1u << kInPosition | 1u << kInNormal | 1u << kInDiffuse | 1u << kInSpecular |
                                    1u << kInFog | 0xFu << kInTexture0;

// Where a draw's vertices are, for the paths on the GPU (whose shaders decode the guest's bytes).
struct VertexSource {
    const std::vector<uint32_t>* indices = nullptr;  // draw_indexed: vertex numbers into the vertex arrays
    const uint8_t* inline_bytes = nullptr;            // draw_inline: the vertices themselves...
    uint32_t inline_stride = 0;
    const uint32_t* inline_offsets = nullptr;         // ...and each attribute's offset in a vertex
};

// Collects what a GPU vertex shader needs to read a draw's vertices: `data` (the guest memory they occupy, as it is),
// `layout` (per attribute, see FixedFunctionConstants::attribute_layout; only those in `attributes` with an array),
// `defaults` and `order` (the triangle list of vertex numbers). False when the vertices cannot be read that way.
bool gather_vertex_data(uint32_t count, const VertexSource& source, uint32_t attributes, int32_t (*layout)[4],
                        float (*defaults)[4], std::vector<uint8_t>& data, std::vector<uint32_t>& order) {
    data.clear();
    order.clear();
    append_triangles(g_k.primitive, count, order);
    for (uint32_t a = 0; a < kVshAttributes; ++a) {
        std::memcpy(defaults[a], g_k.attribute[a].data(), 16);
        const uint32_t format = g_k.array_format[a];
        const bool from_array = ((attributes >> a) & 1) && ((format >> 4) & 0xF);
        layout[a][0] = 0;
        layout[a][1] = static_cast<int32_t>(source.inline_bytes ? source.inline_stride : format >> 8);
        layout[a][2] = static_cast<int32_t>(format & 0xFF);
        layout[a][3] = from_array ? 1 : 0;
    }
    if (source.inline_bytes) {
        for (uint32_t a = 0; a < kVshAttributes; ++a) layout[a][0] = static_cast<int32_t>(source.inline_offsets[a]);
        data.assign(source.inline_bytes, source.inline_bytes + static_cast<size_t>(count) * source.inline_stride);
    } else {
        // Only the part of each array between the lowest and the highest index is uploaded; arrays close together
        // in memory (usually one interleaved buffer) share one copy.
        const std::vector<uint32_t>& indices = *source.indices;
        const auto [low, high] = std::minmax_element(indices.begin(), indices.end());
        struct Range {
            uint32_t start, end, attribute, address;
        };
        Range ranges[kVshAttributes];
        uint32_t used = 0;
        for (uint32_t a = 0; a < kVshAttributes; ++a) {
            if (!layout[a][3]) continue;
            const uint32_t format = g_k.array_format[a], offset = g_k.array_offset[a], stride = format >> 8;
            const uint32_t base = (offset & 0x80000000) ? g_k.dma_vertex_b : g_k.dma_vertex_a;
            const uint64_t address = (base + (offset & 0x7FFFFFFF)) & 0x7FFFFFFF;
            const uint64_t end = address + static_cast<uint64_t>(*high) * stride + attribute_bytes(format);
            if (end > kPhysicalMemorySize) return false;
            const uint32_t start = static_cast<uint32_t>(address + static_cast<uint64_t>(*low) * stride);
            ranges[used++] = {start & ~3u, static_cast<uint32_t>((end + 3) & ~3ull), a, static_cast<uint32_t>(address)};
        }
        std::sort(ranges, ranges + used, [](const Range& x, const Range& y) { return x.start < y.start; });
        constexpr uint32_t kMergeGap = 4096, kMaximumBytes = 32u << 20;
        for (uint32_t i = 0; i < used;) {
            uint32_t j = i + 1, end = ranges[i].end;
            while (j < used && ranges[j].start <= end + kMergeGap) end = std::max(end, ranges[j++].end);
            const size_t at = data.size();
            if (at + (end - ranges[i].start) > kMaximumBytes) return false;
            const uint8_t* bytes = physical_pointer(ranges[i].start);
            data.insert(data.end(), bytes, bytes + (end - ranges[i].start));
            for (uint32_t k = i; k < j; ++k) {
                layout[ranges[k].attribute][0] =
                    static_cast<int32_t>(static_cast<int64_t>(at) + ranges[k].address - ranges[i].start);
            }
            i = j;
        }
        for (uint32_t& vertex : order) vertex = indices[vertex];
    }
    data.resize((data.size() + 3) / 4 * 4 + 8);  // whole words, and room for the shader's two-word reads
    return true;
}

// The fixed-function draw with the transform done by the renderer's vertex shader: here the state is copied into
// the shader's constants. False when the vertices cannot be read by the GPU (the CPU then does the draw).
bool draw_fixed_function_on_gpu(const SurfaceState& target, uint32_t count, const VertexSource& source) {
    LARGE_INTEGER pack_start, pack_end;
    QueryPerformanceCounter(&pack_start);
    static FixedFunctionConstants c;  // GPU thread only
    static std::vector<uint8_t> data;
    static std::vector<uint32_t> order;
    if (!gather_vertex_data(count, source, kGpuAttributes, c.attribute_layout, c.attribute_default, data, order)) {
        return false;
    }

    if (g_k.skin_mode) log_once(0x2001, "fixed-function skinning is not supported yet; the first matrix is used");
    std::memcpy(c.composite, g_k.composite, sizeof(c.composite));
    std::memcpy(c.model_view, g_k.model_view[0], sizeof(c.model_view));
    std::memcpy(c.inverse_model_view, g_k.inverse_model_view[0], sizeof(c.inverse_model_view));
    std::memcpy(c.texture_matrix, g_k.texture_matrix, sizeof(c.texture_matrix));
    std::memcpy(c.texgen_plane, g_k.texgen_plane, sizeof(c.texgen_plane));
    c.texture_matrix_mask = 0;
    for (int stage = 0; stage < 4; ++stage) {
        if (g_k.texture_matrix_enable[stage]) c.texture_matrix_mask |= 1u << stage;
        for (int i = 0; i < 4; ++i) {
            const uint32_t mode = g_k.texgen[stage][i];
            c.texgen_mode[stage][i] = mode == kTexgenEyeLinear ? 1 : mode == kTexgenObjectLinear ? 2 : 0;
            if (mode != kTexgenDisable && !c.texgen_mode[stage][i]) {
                log_once(0x2100 + mode, "fixed-function texgen mode not supported yet");
            }
        }
    }
    std::memcpy(c.light, g_k.light, sizeof(c.light));
    std::memcpy(c.viewport_offset, g_k.viewport_offset, 16);
    std::memcpy(c.eye_position, g_k.eye_position, 16);
    std::memcpy(c.scene_ambient, g_k.scene_ambient, 12);
    std::memcpy(c.material_emission, g_k.material_emission, 12);
    c.material_emission[3] = g_k.material_alpha;
    std::memcpy(c.specular_params, g_k.specular_params, sizeof(g_k.specular_params));
    std::memcpy(c.fog_plane, g_k.fog_plane, 16);
    std::memcpy(c.fog_params, g_k.fog_params, sizeof(g_k.fog_params));
    c.flags = (g_k.lighting ? kFixedFunctionLighting : 0) | (g_k.specular ? kFixedFunctionSpecular : 0) |
              (g_k.fog ? kFixedFunctionFog : 0) | (g_k.normalization ? kFixedFunctionNormalize : 0) |
              ((g_k.light_control & kLightControlSeparateSpecular) ? kFixedFunctionSeparateSpecular : 0) |
              ((g_k.light_control & kLightControlLocalEye) ? kFixedFunctionLocalEye : 0);
    c.color_material = g_k.color_material;
    c.light_enable_mask = g_k.light_enable_mask;
    c.fog_gen_mode = g_k.fog_gen_mode;
    c.fog_mode = g_k.fog_mode;
    c.clip_max = g_k.clip_max;
    if (g_k.fog) {
        const uint32_t generation = g_k.fog_gen_mode;
        if (generation != kFogGenSpecularAlpha && generation != kFogGenRadial && generation != kFogGenPlanar &&
            generation != kFogGenAbsPlanar && generation != kFogGenX) {
            log_once(0x2200 + generation, "fog generation mode not supported yet");
        }
    }

    QueryPerformanceCounter(&pack_end);
    const uint64_t elapsed = static_cast<uint64_t>(pack_end.QuadPart - pack_start.QuadPart);
    renderer_note_vertex_work(elapsed, count);
    debug_vertex_work(2, elapsed, count);
    prepare_draw_state();
    renderer_draw_fixed_function(target, g_k.draw, c, data.data(), static_cast<uint32_t>(data.size()), order);
    return true;
}

// The vertex program draw on the GPU: the program translated to HLSL (once per program), its constant memory and
// the vertices' bytes. False when it cannot run there (the CPU then does the draw).
bool draw_program_on_gpu(const SurfaceState& target, uint32_t count, const VertexSource& source) {
    const VertexProgram& program = g_k.vertex_program;
    if (!renderer_prepare_program(program.key(), [&] { return program.hlsl(); })) return false;
    LARGE_INTEGER pack_start, pack_end;
    QueryPerformanceCounter(&pack_start);
    static ProgramConstants c;  // GPU thread only
    static std::vector<uint8_t> data;
    static std::vector<uint32_t> order;
    if (!gather_vertex_data(count, source, ~0u, c.attribute_layout, c.attribute_default, data, order)) return false;
    static_assert(sizeof(c.constants) == sizeof(g_k.constants));
    std::memcpy(c.constants, g_k.constants, sizeof(c.constants));
    std::memcpy(c.fog_params, g_k.fog_params, sizeof(g_k.fog_params));
    c.fog_enabled = g_k.fog ? 1 : 0;
    c.fog_mode = g_k.fog_mode;
    c.clip_max = g_k.clip_max;
    QueryPerformanceCounter(&pack_end);
    const uint64_t elapsed = static_cast<uint64_t>(pack_end.QuadPart - pack_start.QuadPart);
    renderer_note_vertex_work(elapsed, count);
    debug_vertex_work(3, elapsed, count);
    prepare_draw_state();
    renderer_draw_program(target, g_k.draw, program.key(), c, data.data(), static_cast<uint32_t>(data.size()), order);
    return true;
}

// Runs the vertex program over `count` vertices obtained from `fetch` and draws the primitive.
template <typename Fetch>
void draw(uint32_t count, const VertexSource& source, Fetch fetch) {
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

    if (gpu_fixed_function() && !g_debug_window_open &&
        (fixed_function ? draw_fixed_function_on_gpu(target, count, source) : draw_program_on_gpu(target, count, source))) {
        ++g_draws;
        update_debug_window();
        return;
    }

    std::vector<RenderVertex> vertices(count);
    LARGE_INTEGER vertex_start, vertex_end;
    QueryPerformanceCounter(&vertex_start);
    const auto transform = [&](uint32_t first, uint32_t end) {
        Vec4 inputs[kVshAttributes];
        Vec4 outputs[kVshOutputs];
        for (uint32_t i = first; i < end; ++i) {
            std::copy(std::begin(g_k.attribute), std::end(g_k.attribute), inputs);
            fetch(i, inputs);
            if (fixed_function) {
                transform_fixed_function(inputs, outputs);
            } else {
                g_k.vertex_program.execute(inputs, g_k.constants, outputs);
            }
            outputs[kOutFog][0] = fog_factor(outputs[kOutFog][0]);
            vertices[i] = to_render_vertex(outputs);
        }
    };
    constexpr uint32_t kParallelVertices = 192;  // below this, waking the helpers costs more than it saves
    if (count >= kParallelVertices) {
        g_vertex_workers.run(count, transform);
    } else {
        transform(0, count);
    }
    QueryPerformanceCounter(&vertex_end);
    renderer_note_vertex_work(static_cast<uint64_t>(vertex_end.QuadPart - vertex_start.QuadPart), count);
    debug_vertex_work(fixed_function ? 0 : 1, static_cast<uint64_t>(vertex_end.QuadPart - vertex_start.QuadPart), count);
    static bool fixed_function_only = false;  // MC3_DEBUG_DRAWS_FF=1: describe only fixed-function draws
    if (g_debug_draws < 0) {
        char value[16] = {};
        g_debug_draws = GetEnvironmentVariableA("MC3_DEBUG_DRAWS", value, sizeof(value)) ? std::atoi(value) : 0;
        fixed_function_only = GetEnvironmentVariableA("MC3_DEBUG_DRAWS_FF", value, sizeof(value)) != 0;
    }
    if (g_debug_draws > 0 && (fixed_function || !fixed_function_only)) {
        --g_debug_draws;
        log_line("draw %llu: primitive %u, %u vertices, surface %08X %ux%u (clip %u,%u %ux%u), clip max %g",
                 static_cast<unsigned long long>(g_draws), g_k.primitive, count, target.color_address, target.width,
                 target.height, target.clip_x, target.clip_y, target.clip_width, target.clip_height, g_k.clip_max);
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
    // MC3_SKIP=<texture>[:<target>[:<blend destination factor>]] (hex, 0 = any): skip the matching draws, to find
    // which one makes a visual defect. Texture addresses match by 4 KB page.
    static const std::array<uint32_t, 3> skip = [] {
        std::array<uint32_t, 3> s{~0u, 0, ~0u};
        char value[64] = {};
        if (GetEnvironmentVariableA("MC3_SKIP", value, sizeof(value))) {
            unsigned texture = 0, target = 0, blend = ~0u;
            const int fields = std::sscanf(value, "%x:%x:%x", &texture, &target, &blend);
            s = {texture, fields >= 2 ? target : 0u, fields >= 3 ? blend : ~0u};
        }
        return s;
    }();
    if (skip[0] != ~0u) {
        const bool texture_matches = skip[0] == 0 || (g_k.draw.textures[0].enabled &&
                                                      (g_k.draw.textures[0].descriptor.address & ~0xFFFu) == (skip[0] & ~0xFFFu));
        const bool target_matches = skip[1] == 0 || target.color_address == skip[1];
        const bool blend_matches = skip[2] == ~0u || g_k.draw.blend_destination == skip[2];
        if (texture_matches && target_matches && blend_matches) {
            ++g_draws;
            return;
        }
    }
    // MC3_DUMP_DRAW=<texture>:<target>:<blend destination> (like MC3_SKIP): during the MC3_DEBUG_FRAME_AT window,
    // the first matching draw saves its texture and its target before and after it (dump-*.bmp).
    static const std::array<uint32_t, 3> dump_spec = [] {
        std::array<uint32_t, 3> s{~0u, 0, ~0u};
        char value[64] = {};
        if (GetEnvironmentVariableA("MC3_DUMP_DRAW", value, sizeof(value))) {
            unsigned texture = 0, target_address = 0, blend = ~0u;
            const int fields = std::sscanf(value, "%x:%x:%x", &texture, &target_address, &blend);
            s = {texture, fields >= 2 ? target_address : 0u, fields >= 3 ? blend : ~0u};
        }
        return s;
    }();
    const bool dump_now = !g_draw_dumped && dump_spec[0] != ~0u && g_debug_window_open &&
                          (dump_spec[0] == 0 || (g_k.draw.textures[0].enabled &&
                                                 (g_k.draw.textures[0].descriptor.address & ~0xFFFu) == (dump_spec[0] & ~0xFFFu))) &&
                          (dump_spec[1] == 0 || target.color_address == dump_spec[1]) &&
                          (dump_spec[2] == ~0u || g_k.draw.blend_destination == dump_spec[2]);
    if (dump_now) {
        g_draw_dumped = true;
        log_line("dump draw %llu", static_cast<unsigned long long>(g_draws));
        renderer_dump_target(g_k.draw.textures[0].descriptor.address, "dump-texture");
        renderer_dump_target(target.color_address, "dump-before");
    }
    renderer_draw(target, g_k.draw, triangles);
    if (dump_now) renderer_dump_target(target.color_address, "dump-after");
    ++g_draws;

    update_debug_window();
    if (g_debug_window_open) {
        // MC3_DEBUG_PROBE=x,y: log the screen pixel there whenever a draw changes it.
        if (g_probe_x < 0) {
            char value[32] = {};
            if (GetEnvironmentVariableA("MC3_DEBUG_PROBE", value, sizeof(value))) {
                std::sscanf(value, "%d,%d", &g_probe_x, &g_probe_y);
            } else {
                g_probe_x = g_probe_y = 100000;
            }
        }
        uint8_t pixel[4];
        if (g_probe_x < 100000 && renderer_probe(target.color_address, static_cast<uint32_t>(g_probe_x),
                                                 static_cast<uint32_t>(g_probe_y), pixel)) {
            uint32_t value;
            std::memcpy(&value, pixel, 4);
            uint32_t& last = g_last_probe[target.color_address == 0x03B84000 ? 0 : 1];
            if (value != last) {
                log_line("frame probe %llu: surf %08X pixel BGRA %02X %02X %02X alpha %02X",
                         static_cast<unsigned long long>(g_draws), target.color_address, pixel[0], pixel[1], pixel[2],
                         pixel[3]);
                last = value;
            }
        }
        const DrawState& d = g_k.draw;
        const RenderVertex& v = vertices[0];
        // Screen rectangle covered by the vertices (pixels of the target surface).
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
        for (const RenderVertex& p : vertices) {
            u0 = std::min(u0, p.texture[0][0]), v0 = std::min(v0, p.texture[0][1]);
            u1 = std::max(u1, p.texture[0][0]), v1 = std::max(v1, p.texture[0][1]);
            if (p.position[3] <= 0) continue;
            const float x = p.position[0] / p.position[3];
            const float y = p.position[1] / p.position[3];
            x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
        }
        log_line("frame rect %llu: %.0f %.0f %.0f %.0f uv %.3f %.3f %.3f %.3f texmatrix %d texgen %X address %08X "
                 "filter %08X",
                 static_cast<unsigned long long>(g_draws), x0, y0, x1, y1, u0, v0, u1, v1,
                 g_k.texture_matrix_enable[0], g_k.texgen[0][0], g_k.draw.textures[0].address_mode,
                 g_k.draw.textures[0].filter);
        log_line("frame draw %llu: %s prim %u n %u surf %08X | tex0 %s %08X fmt %08X | blend %d %X %X | z %d %X w %d | "
                 "atest %d %X ref %02X | mask %08X | clip (%.3g %.3g %.3g %.3g) d0 (%.2f %.2f %.2f %.2f) | comb %08X %08X %08X"
                 " | stages %05X ctl %08X | alpha %08X %08X out %08X | stencil %d %X ref %02X rm %02X wm %02X ops %X %X %X"
                 " | cull %d %X front %X eq %X",
                 static_cast<unsigned long long>(g_draws), fixed_function ? "ff" : "vp", g_k.primitive, count,
                 target.color_address, d.textures[0].enabled ? "on" : "off", d.textures[0].descriptor.address,
                 d.textures[0].descriptor.format, d.blend_enable, d.blend_source, d.blend_destination, d.depth_test,
                 d.depth_function, d.depth_write, g_k.alpha_test, g_k.alpha_function, g_k.alpha_reference,
                 d.color_mask, v.position[0] / v.position[3], v.position[1] / v.position[3],
                 v.position[2] / v.position[3], v.position[3], v.diffuse[0], v.diffuse[1], v.diffuse[2],
                 v.diffuse[3], d.program.color_input[0], d.program.final0, d.program.final1, d.program.stage_program,
                 d.program.control, d.program.alpha_input[0], d.program.alpha_output[0], d.program.color_output[0],
                 d.stencil_test, d.stencil_function, d.stencil_reference, d.stencil_read_mask, d.stencil_write_mask,
                 d.stencil_fail, d.stencil_depth_fail, d.stencil_pass, d.cull_enable, d.cull_face, d.front_face,
                 d.blend_equation);
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
    draw(count, VertexSource{nullptr, bytes, stride, offsets}, [&](uint32_t vertex, Vec4* inputs) {
        for (uint32_t a = 0; a < kVshAttributes; ++a) {
            if ((g_k.array_format[a] >> 4) & 0xF) inputs[a] = read_attribute(bytes + vertex * stride + offsets[a], g_k.array_format[a]);
        }
    });
}

void draw_indexed(const std::vector<uint32_t>& indices) {
    draw(static_cast<uint32_t>(indices.size()), VertexSource{&indices}, [&](uint32_t vertex, Vec4* inputs) {
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

uint32_t kelvin_drawn_width(uint32_t address) {
    const auto found = g_drawn_widths.find(address & 0x0FFFFFFF);
    return found != g_drawn_widths.end() ? found->second : 0;
}

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
    // The fixed-function state lives in the same constant memory the vertex programs read (NV_IGRAPH_XF_XFCTX_*):
    // D3D's vertex shaders end with a screen transform that uses the viewport scale and offset as c[-38], c[-37].
    if (method >= 0x0680 && method < 0x06C0) {
        const uint32_t i = (method - 0x0680) / 4;
        g_k.composite[i] = as_float(argument);
        g_k.constants[kXfComposite + i / 4][i % 4] = as_float(argument);
        return true;
    }
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
    if (method >= 0x0A20 && method < 0x0A30) {
        g_k.viewport_offset[(method - 0x0A20) / 4] = as_float(argument);
        g_k.constants[kXfViewportOffset][(method - 0x0A20) / 4] = as_float(argument);
        return true;
    }
    if (method >= 0x0AF0 && method < 0x0B00) {
        g_k.viewport_scale[(method - 0x0AF0) / 4] = as_float(argument);
        g_k.constants[kXfViewportScale][(method - 0x0AF0) / 4] = as_float(argument);
        return true;
    }
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
    if (method >= 0x0A50 && method < 0x0A60) {
        g_k.eye_position[(method - 0x0A50) / 4] = as_float(argument);
        g_k.constants[kXfEyePosition][(method - 0x0A50) / 4] = as_float(argument);
        return true;
    }

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
        case 0x0308: d.cull_enable = argument != 0; return true;
        case 0x039C: d.cull_face = argument; return true;
        case 0x03A0: d.front_face = argument; return true;
        case 0x032C: d.stencil_test = argument != 0; return true;
        case 0x0360: d.stencil_write_mask = argument & 0xFF; return true;
        case 0x0364: d.stencil_function = argument; return true;
        case 0x0368: d.stencil_reference = argument & 0xFF; return true;
        case 0x036C: d.stencil_read_mask = argument & 0xFF; return true;
        case 0x0370: d.stencil_fail = argument; return true;
        case 0x0374: d.stencil_depth_fail = argument; return true;
        case 0x0378: d.stencil_pass = argument; return true;
        case 0x037C: return true;  // SET_SHADE_MODE: Gouraud is what D3D11 does
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
            {
                static std::set<uint64_t> seen;
                const uint64_t key = (static_cast<uint64_t>(argument) << 32) | g_k.color_clear;
                if (seen.size() < 64 && seen.insert(key).second) {
                    log_line("nv2a: clear flags %02X colour %08X zeta %08X surface %08X", argument, g_k.color_clear,
                             g_k.zstencil_clear, surface().color_address);
                }
            }
            if (g_debug_window_open) {
                log_line("frame clear: flags %02X colour %08X zeta %08X rect %u %u %u %u surf %08X", argument,
                         g_k.color_clear, g_k.zstencil_clear, g_k.clear_horizontal & 0xFFFF,
                         g_k.clear_vertical & 0xFFFF, g_k.clear_horizontal >> 16, g_k.clear_vertical >> 16,
                         surface().color_address);
            }
            renderer_clear(surface(), argument, g_k.color_clear, g_k.zstencil_clear, g_k.clear_horizontal & 0xFFFF,
                           g_k.clear_vertical & 0xFFFF, g_k.clear_horizontal >> 16, g_k.clear_vertical >> 16);
            return true;
        default:
            return false;
    }
}

}  // namespace mc3::pc
