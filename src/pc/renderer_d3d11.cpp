#include "pc/renderer_d3d11.h"

#include <d3d11_1.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "pc/log.h"
#include "pc/paths.h"
#include "pc/physical_memory.h"
#include "pc/profile.h"
#include "pc/settings.h"
#include "pc/texture_page_hash.h"
#include "pc/texture_upscale.h"

namespace mc3::pc {

using Microsoft::WRL::ComPtr;

namespace {

constexpr char kVertexShader[] = R"(
struct VSInput {
    float4 position : POSITION;
    float4 d0 : COLOR0;
    float4 d1 : COLOR1;
    float4 t0 : TEXCOORD0;
    float4 t1 : TEXCOORD1;
    float4 t2 : TEXCOORD2;
    float4 t3 : TEXCOORD3;
    float fog : FOG;
};
struct PSInput {
    float4 position : SV_Position;
    float4 d0 : COLOR0;
    float4 d1 : COLOR1;
    float4 t0 : TEXCOORD0;
    float4 t1 : TEXCOORD1;
    float4 t2 : TEXCOORD2;
    float4 t3 : TEXCOORD3;
    float fog : FOG;
};
// Positions arrive in pixels of the render target times w; scale = (2 / width, 2 / height, the HUD's narrowing
// across, the pixel column it narrows towards).
cbuffer Surface : register(b0) { float4 scale; };
PSInput main(VSInput i) {
    PSInput o;
    const float x = (i.position.x - scale.w * i.position.w) * scale.z + scale.w * i.position.w;
    o.position = float4(x * scale.x - i.position.w, i.position.w - i.position.y * scale.y, i.position.z, i.position.w);
    o.d0 = i.d0;
    o.d1 = i.d1;
    o.t0 = i.t0;
    o.t1 = i.t1;
    o.t2 = i.t2;
    o.t3 = i.t3;
    o.fog = i.fog;
    return o;
}
)";

// The start of the vertex shaders that read the guest's vertex bytes as they are (fixed function and vertex
// programs): attribute decoding, fog factor and final position, the same computations as read_attribute,
// fog_factor and to_render_vertex in nv2a_kelvin.cpp (keep them in step). Each shader's own constants (b1) follow.
constexpr char kVertexDataShader[] = R"(
struct PSInput {
    float4 position : SV_Position;
    float4 d0 : COLOR0;
    float4 d1 : COLOR1;
    float4 t0 : TEXCOORD0;
    float4 t1 : TEXCOORD1;
    float4 t2 : TEXCOORD2;
    float4 t3 : TEXCOORD3;
    float fog : FOG;
};
cbuffer Surface : register(b0) { float4 scale; };
cbuffer Draw : register(b2) { uint first; };  // byte offset of this draw's vertex data (changes every draw)
ByteAddressBuffer vertex_data : register(t0);

// From the bits rather than isnan, isinf and isfinite, which Wine's HLSL compiler lacks.
bool is_nan(float x) { return (asuint(x) & 0x7FFFFFFF) > 0x7F800000; }
bool is_inf(float x) { return (asuint(x) & 0x7FFFFFFF) == 0x7F800000; }
bool is_finite(float x) { return (asuint(x) & 0x7F800000) != 0x7F800000; }

uint load32(uint address) {  // attributes are not always 4-byte aligned (3 shorts)
    const uint aligned = address & ~3u, shift = (address & 3u) * 8;
    const uint low = vertex_data.Load(aligned);
    return shift ? (low >> shift) | (vertex_data.Load(aligned + 4) << (32 - shift)) : low;
}
float signed_bits(uint value, uint bits) {
    const int shifted = (int)(value << (32 - bits)) >> (32 - bits);
    return max(-1, shifted / (float)((1 << (bits - 1)) - 1));
}
// l: byte offset of vertex 0, stride, format, from the data (0: `fallback`, the attribute without an array).
float4 decode_attribute(uint vertex, int4 l, float4 fallback) {
    if (l.w == 0) return fallback;
    const uint address = (uint)((int)first + l.x + (int)vertex * l.y);
    const uint type = l.z & 15, size = (l.z >> 4) & 15;
    float4 decoded = float4(0, 0, 0, 1);
    if (type == 0 || type == 4) {  // UB_D3D (B G R A in memory), UB_OGL
        const uint w = load32(address);
        decoded = float4(w & 255, (w >> 8) & 255, (w >> 16) & 255, w >> 24) / 255;
        if (type == 0 && size >= 4) return decoded.zyxw;
    } else if (type == 1 || type == 5) {  // S1 (normalised), S32K
        const uint w0 = load32(address), w1 = size > 2 ? load32(address + 4) : 0;
        decoded = float4((int)(w0 << 16) >> 16, (int)w0 >> 16, (int)(w1 << 16) >> 16, (int)w1 >> 16);
        if (type == 1) decoded = max(-1, decoded / 32767);
    } else if (type == 2) {  // F
        [unroll] for (uint i = 0; i < 4; ++i) decoded[i] = i < size ? asfloat(load32(address + i * 4)) : 0;
    } else if (type == 6) {  // CMP: 11, 11, 10 signed bits
        const uint p = load32(address);
        return float4(signed_bits(p & 0x7FF, 11), signed_bits((p >> 11) & 0x7FF, 11), signed_bits(p >> 22, 10), 1);
    } else {
        return float4(0, 0, 0, 1);
    }
    float4 v = float4(0, 0, 0, 1);
    [unroll] for (uint i = 0; i < 4; ++i) if (i < size) v[i] = decoded[i];
    return v;
}

float fog_factor(float distance, bool enabled, uint mode, float4 params) {
    if (!enabled) return 1;
    if (is_inf(distance)) return mode == 0x801 || mode == 0x803 ? 0 : 1;
    float factor = 1;
    if (mode == 0x2601 || mode == 0x804) factor = params.x + distance * params.y - 1;
    else if (mode == 0x800 || mode == 0x802) factor = params.x + exp2(distance * params.y * 16) - 1.5;
    else if (mode == 0x801 || mode == 0x803) factor = params.x + exp2(-distance * distance * params.y * params.y * 32) - 1.5;
    if (mode == 0x802 || mode == 0x803 || mode == 0x804) factor = abs(factor);
    return is_nan(factor) ? 1 : factor;
}

// A screen-space position (pixels; w apart) to clip space, as to_render_vertex and then the main vertex shader do.
float4 clip_position(float4 screen, float clip_max) {
    float w = screen.w;
    if (!is_finite(w) || abs(w) < 5.42101e-20) w = w < 0 ? -5.42101e-20 : 5.42101e-20;
    const float z = clip_max != 0 ? screen.z / clip_max : screen.z;
    const float x = (screen.x - scale.w) * scale.z + scale.w;
    return float4(x * w * scale.x - w, w - screen.y * w * scale.y, z * w, w);
}
)";

// The fixed-function transform on the GPU: the same computations as transform_fixed_function and light_vertex in
// nv2a_kelvin.cpp (keep them in step). Follows kVertexDataShader.
constexpr char kFixedFunctionVertexShader[] = R"(
cbuffer FixedFunction : register(b1) {
    float4 composite[4];
    float4 model_view[4];
    float4 inverse_model_view[4];
    float4 texture_matrix[16];
    float4 texgen_plane[16];
    int4 texgen_mode[4];
    float4 attribute_default[16];
    int4 attribute_layout[16];  // byte offset of vertex 0, stride, format, from the data
    float4 light[64];
    float4 viewport_offset;
    float4 eye_position;
    float4 scene_ambient;
    float4 material_emission;
    float4 specular_params[2];
    float4 fog_plane;
    float4 fog_params;
    uint4 settings;  // flags, colour material, light enable mask, fog generation mode
    uint fog_mode;
    uint texture_matrix_mask;
    uint padding;
    float clip_max;
};

static const uint kLighting = 1, kSpecular = 2, kFog = 4, kNormalize = 8, kSeparateSpecular = 16, kLocalEye = 32;

float4 attribute(uint vertex, uint a) { return decode_attribute(vertex, attribute_layout[a], attribute_default[a]); }
float4 rows4(float4 r0, float4 r1, float4 r2, float4 r3, float4 v) {
    return float4(dot(r0, v), dot(r1, v), dot(r2, v), dot(r3, v));
}
float light_value(uint i, uint k) { return light[i * 8 + k / 4][k % 4]; }
float3 light_vector(uint i, uint k) { return float3(light_value(i, k), light_value(i, k + 1), light_value(i, k + 2)); }
float specular_value(uint k) { return specular_params[k / 4][k % 4]; }
float3 safe_normalize(float3 v) { const float l = length(v); return l > 0 ? v / l : v; }

PSInput main(uint vertex : SV_VertexID) {
    const float4 position = attribute(vertex, 0);
    const float4 normal_in = attribute(vertex, 2);
    const float4 diffuse = attribute(vertex, 3);
    const float4 specular = attribute(vertex, 4);
    const float4 eye = rows4(model_view[0], model_view[1], model_view[2], model_view[3], position);
    const float4 p = rows4(composite[0], composite[1], composite[2], composite[3], position);
    float w = p.w;
    if (!is_finite(w) || abs(w) < 5.42101e-20) w = w < 0 ? -5.42101e-20 : 5.42101e-20;
    const float3 screen = float3(p.x / w + viewport_offset.x, p.y / w + viewport_offset.y, p.z / w);

    float4 out_diffuse, out_specular;
    if (settings.x & kLighting) {
        const uint material = settings.y;
        const uint emission_source = material & 3, ambient_source = (material >> 2) & 3,
                   diffuse_source = (material >> 4) & 3, specular_source = (material >> 6) & 3;
        const float3 emission_factor = material_emission.xyz;
        float3 front;
        if (emission_source == 0 || ambient_source == 0) {
            front = scene_ambient.xyz;
            if (emission_source != 0) front += (emission_source == 1 ? diffuse.rgb : specular.rgb) * emission_factor;
            if (ambient_source != 0) front += (ambient_source == 1 ? diffuse.rgb : specular.rgb) * emission_factor;
        } else {
            front = (emission_source == 1 ? diffuse.rgb : specular.rgb) +
                    (ambient_source == 1 ? diffuse.rgb : specular.rgb) * emission_factor;
        }
        const float alpha = diffuse_source == 0 ? material_emission.w : diffuse_source == 1 ? diffuse.a : specular.a;
        float3 lit_specular = 0;
        float3 normal = rows4(inverse_model_view[0], inverse_model_view[1], inverse_model_view[2],
                              inverse_model_view[3], float4(normal_in.xyz, 0)).xyz;
        if (settings.x & kNormalize) normal = safe_normalize(normal);
        const float ew = eye.w != 0 ? eye.w : 1;
        const float3 eye_point = eye.xyz / ew;
        const bool local_eye = (settings.x & kLocalEye) != 0;
        const float3 to_eye = safe_normalize(eye_position.xyz - eye_point);
        [loop] for (uint i = 0; i < 8; ++i) {
            const uint type = (settings.z >> (i * 2)) & 3;
            if (type == 0) continue;
            float attenuation = 1;
            float3 direction, half_vector;
            if (type == 1) {
                direction = safe_normalize(light_vector(i, 13));
                half_vector = local_eye ? safe_normalize(direction + to_eye) : light_vector(i, 10);
            } else {
                const float3 to_light = light_vector(i, 23) - eye_point;
                const float d = length(to_light);
                if (d > light_value(i, 9)) continue;
                direction = safe_normalize(to_light);
                const float denominator = light_value(i, 26) + light_value(i, 27) * d + light_value(i, 28) * d * d;
                attenuation = denominator != 0 ? 1 / denominator : 1;
                half_vector = safe_normalize(direction + (local_eye ? to_eye : float3(0, 0, 0)));
                if (type == 3) {
                    const float cosine = light_value(i, 19) * direction.x + light_value(i, 20) * direction.y +
                                         light_value(i, 21) * direction.z + light_value(i, 22);
                    float spot = 1;
                    if (cosine <= 0) {
                        spot = 0;
                    } else if (cosine < 1 && cosine > -light_value(i, 16)) {
                        const float denom = light_value(i, 17) * cosine + light_value(i, 18);
                        spot = denom != 0 ? saturate((cosine + light_value(i, 16)) / denom) : 1;
                    }
                    attenuation *= spot;
                }
            }
            const float n_dot_l = max(0, dot(normal, direction));
            const float n_dot_h = max(0, dot(normal, half_vector));
            float power = 0;
            if (n_dot_l > 0 && n_dot_h > -specular_value(0)) {
                const float denominator = specular_value(1) * n_dot_h + specular_value(2);
                power = denominator == 0 ? 10000 : denominator > 0 ? clamp((n_dot_h + specular_value(0)) / denominator, 0, 10000) : 0;
            }
            front += light_vector(i, 0) * attenuation;
            float3 diffuse_term = light_vector(i, 3) * (attenuation * n_dot_l);
            if (diffuse_source != 0) diffuse_term *= diffuse_source == 1 ? diffuse.rgb : specular.rgb;
            front += diffuse_term;
            float3 specular_term = light_vector(i, 6) * (attenuation * power);
            if (specular_source != 0) specular_term *= specular_source == 1 ? diffuse.rgb : specular.rgb;
            lit_specular += specular_term;
        }
        out_diffuse = float4(front, alpha);
        if ((settings.x & kSpecular) == 0) {
            out_specular = float4(0, 0, 0, 1);
        } else if (settings.x & kSeparateSpecular) {
            out_specular = float4(lit_specular, specular.a);
        } else {
            out_diffuse = float4(front + lit_specular, alpha);
            out_specular = specular;
        }
    } else {
        out_diffuse = diffuse;
        out_specular = (settings.x & kSpecular) ? specular : float4(0, 0, 0, 1);
    }

    float4 textures[4];
    [unroll] for (uint stage = 0; stage < 4; ++stage) {
        float4 t = attribute(vertex, 9 + stage);
        [unroll] for (uint c = 0; c < 4; ++c) {
            const int mode = texgen_mode[stage][c];
            if (mode == 1) t[c] = dot(texgen_plane[stage * 4 + c], eye);
            else if (mode == 2) t[c] = dot(texgen_plane[stage * 4 + c], position);
        }
        if (texture_matrix_mask & (1u << stage)) {
            t = rows4(texture_matrix[stage * 4], texture_matrix[stage * 4 + 1], texture_matrix[stage * 4 + 2],
                      texture_matrix[stage * 4 + 3], t);
        }
        textures[stage] = t;
    }

    float fog = 0;
    if (settings.x & kFog) {
        const float planar = dot(fog_plane.xyz, eye.xyz) + fog_plane.w;
        const uint generation = settings.w;
        if (generation == 0) fog = saturate(specular.a);
        else if (generation == 1) fog = length(eye.xyz);
        else if (generation == 2) fog = planar;
        else if (generation == 3) fog = abs(planar);
        else if (generation == 6) fog = attribute(vertex, 5).x;
    }

    PSInput o;
    o.position = clip_position(float4(screen, w), clip_max);
    o.d0 = out_diffuse;
    o.d1 = out_specular;
    o.t0 = textures[0];
    o.t1 = textures[1];
    o.t2 = textures[2];
    o.t3 = textures[3];
    o.fog = fog_factor(fog, (settings.x & kFog) != 0, fog_mode, fog_params);
    return o;
}
)";

// The start of a vertex program's shader (follows kVertexDataShader); the program's own code, generated by
// VertexProgram::hlsl, comes after. The helpers mirror the special cases of nv2a_vsh.cpp.
constexpr char kVertexProgramShader[] = R"(
cbuffer Program : register(b1) {
    float4 c[192];
    float4 attribute_default[16];
    int4 attribute_layout[16];
    float4 fog_params;
    uint fog_enabled;
    uint fog_mode;
    uint padding;
    float clip_max;
};

static const float kInfinity = asfloat(0x7F800000);

float4 attribute(uint vertex, uint a) { return decode_attribute(vertex, attribute_layout[a], attribute_default[a]); }
// 0 * anything is 0 on NV2A, even for infinities, which programs rely on.
float4 mul0(float4 a, float4 b) { return (a == 0 || b == 0) ? 0 : a * b; }
float4 rcc(float s) {
    const float r = 1 / s;
    if (is_nan(r)) return 5.42101e-20;
    const float magnitude = clamp(abs(r), 5.42101e-20, 1.884467e+19);
    return (asuint(r) >> 31) ? -magnitude : magnitude;
}
float4 rsq(float s) { return s == 0 ? kInfinity : is_inf(s) ? 0 : 1 / sqrt(abs(s)); }
float4 exp_ilu(float s) { const float f = floor(s); return float4(exp2(f), s - f, exp2(s), 1); }
float4 log_ilu(float s) {
    const float t = abs(s);
    if (t == 0) return float4(-kInfinity, 1, -kInfinity, 1);
    const float e = floor(log2(t));
    return float4(e, t / exp2(e), log2(t), 1);
}
float4 lit(float4 v) {
    const float epsilon = 1.0 / 256;
    const float x = max(v.x, 0), y = max(v.y, 0), w = clamp(v.w, -(128 - epsilon), 128 - epsilon);
    return float4(1, x, x > 0 ? exp2(w * log2(y)) : 0, 1);
}
)";

struct Target {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;  // R16G16B16A16_FLOAT once a signed blend equation is used
};

struct DepthTarget {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> srv;  // the depth, read by the ambient occlusion pass
    uint32_t width = 0, height = 0;
};

struct CachedTexture {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    uint64_t content_hash = 0;
    uint32_t checked_generation = 0;  // memory unchanged since this write-tracking generation
    TexturePageHashes page_hashes;
    bool waiting = false;            // sharp_2d: the enlarged version is being made (Enlarger)
};

// By address, format, pitch, rectangle, palette and whether it is enlarged for 2D (setting sharp_2d).
using TextureKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, bool>;

// sharp_2d: textures are enlarged on a thread of their own (texture_upscale.cpp), the original showing meanwhile:
// enlarging the radar's map at the start of a race all at once held the game up for a third of a second.
class Enlarger {
public:
    void request(const TextureKey& key, uint64_t hash, const TextureImage& image) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            std::thread([this] { run(); }).detach();
            started_ = true;
        }
        queue_.push_back({key, hash, image});
        wake_.notify_one();
    }
    // True once the texture `key` with content `hash` is done; `levels` empty when it was not worth enlarging. A
    // result for content since replaced is dropped (a new one is on its way).
    bool take(const TextureKey& key, uint64_t hash, std::vector<TextureImage>& levels) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = done_.find(key);
        if (found == done_.end()) return false;
        const bool current = found->second.first == hash;
        if (current) levels = std::move(found->second.second);
        done_.erase(found);
        return current;
    }

private:
    struct Job {
        TextureKey key;
        uint64_t hash;
        TextureImage image;
    };
    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return !queue_.empty(); });
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            std::vector<TextureImage> levels = upscale_2d_texture(job.image);
            std::lock_guard<std::mutex> lock(mutex_);
            done_[job.key] = {job.hash, std::move(levels)};
        }
    }
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> queue_;
    std::map<TextureKey, std::pair<uint64_t, std::vector<TextureImage>>> done_;
    bool started_ = false;
};
Enlarger g_enlarger;

ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;
ComPtr<ID3D11DeviceContext1> g_context1;
ComPtr<IDXGISwapChain1> g_swap_chain;
uint32_t g_swap_width = 0, g_swap_height = 0;
ComPtr<ID3D11VertexShader> g_vertex_shader;
ComPtr<ID3D11InputLayout> g_input_layout;
ComPtr<ID3D11VertexShader> g_fixed_function_vertex_shader;  // null: the transform stays on the CPU
ComPtr<ID3D11Buffer> g_fixed_function_constants;
ComPtr<ID3D11Buffer> g_fixed_function_draw_constants;  // where the draw's vertex data starts
ComPtr<ID3D11Buffer> g_program_constants;              // the vertex programs' constants (ProgramConstants)
ComPtr<ID3D11Buffer> g_vertex_buffer;
uint32_t g_vertex_capacity = 0;
uint32_t g_vertex_used = 0;  // bytes of the ring buffer written since the last DISCARD
ComPtr<ID3D11Buffer> g_constants;
ComPtr<ID3D11RasterizerState> g_rasterizer;  // no culling (clears, copies)
std::map<std::pair<D3D11_CULL_MODE, bool>, ComPtr<ID3D11RasterizerState>> g_cull_rasterizers;

// MC3_FLIP_WINDING=1 inverts how NV2A's front-face winding maps onto D3D11 (for checking the convention).
const bool g_flip_winding = GetEnvironmentVariableW(L"MC3_FLIP_WINDING", nullptr, 0) != 0;

ID3D11RasterizerState* rasterizer_for(const DrawState& state) {
    if (!state.cull_enable) return g_rasterizer.Get();
    constexpr uint32_t kCullFront = 0x404, kCullBack = 0x405, kFrontCcw = 0x901;
    const D3D11_CULL_MODE mode = state.cull_face == kCullFront  ? D3D11_CULL_FRONT
                                 : state.cull_face == kCullBack ? D3D11_CULL_BACK
                                                                : D3D11_CULL_NONE;  // FRONT_AND_BACK: see below
    // Positions keep NV2A's screen orientation (y down) through clip space and the viewport, so the winding the
    // game states is the winding D3D11 sees.
    const bool front_ccw = (state.front_face == kFrontCcw) != g_flip_winding;
    ComPtr<ID3D11RasterizerState>& cached = g_cull_rasterizers[{mode, front_ccw}];
    if (!cached) {
        D3D11_RASTERIZER_DESC desc{};
        desc.FillMode = D3D11_FILL_SOLID;
        desc.CullMode = mode;
        desc.FrontCounterClockwise = front_ccw;
        desc.DepthClipEnable = TRUE;
        desc.ScissorEnable = TRUE;
        g_device->CreateRasterizerState(&desc, &cached);
    }
    return cached.Get();
}
ComPtr<ID3D11Buffer> g_surface_constants;  // vertex shader: pixel-to-clip scale of the current target
uint32_t g_surface_scale_width = 0, g_surface_scale_height = 0;
float g_surface_scale_narrow = 1, g_surface_scale_anchor = 0;  // hud_narrowing
ComPtr<ID3D11VertexShader> g_clear_vertex_shader;
ComPtr<ID3D11PixelShader> g_clear_pixel_shader;
ComPtr<ID3D11Buffer> g_clear_constants;
std::map<uint32_t, ComPtr<ID3D11BlendState>> g_clear_blend_states;  // by write mask
ComPtr<ID3D11Texture2D> g_upload;  // guest framebuffers that were never rendered to
ComPtr<ID3D11ShaderResourceView> g_upload_srv;
uint32_t g_upload_width = 0, g_upload_height = 0;

// Presentation: the frame is drawn into the window keeping the game's 4:3 shape, black bars around it.
HWND g_window_handle = nullptr;
// The finished window picture (effects and overlay), drawn by the GPU thread; a thread of its own copies it into the
// swap chain and presents it, so a frame limiter or overlay tool (RTSS) waiting inside Present holds up that thread
// only, not the emulated GPU (issues #3, #6).
ComPtr<ID3D11Texture2D> g_final_texture, g_back_buffer;
ComPtr<ID3D11RenderTargetView> g_final_rtv;
std::mutex g_present_mutex;  // the final picture: being drawn or being copied
std::mutex g_swap_mutex;     // the swap chain: presenting or resizing
std::condition_variable g_present_wake;
bool g_present_pending = false, g_present_threaded = false;
ComPtr<ID3D11VertexShader> g_present_vertex_shader;
ComPtr<ID3D11PixelShader> g_present_pixel_shader;
ComPtr<ID3D11SamplerState> g_present_sampler;

std::unordered_map<uint32_t, Target> g_targets;
std::unordered_map<uint64_t, DepthTarget> g_depth_targets;
std::unordered_map<uint64_t, ComPtr<ID3D11PixelShader>> g_pixel_shaders;
std::map<TextureKey, CachedTexture> g_textures;
std::map<std::tuple<bool, uint32_t, uint32_t, uint32_t, uint32_t>, ComPtr<ID3D11BlendState>> g_blend_states;
std::map<std::tuple<bool, uint32_t, bool, bool, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>,
         ComPtr<ID3D11DepthStencilState>>
    g_depth_states;
std::map<std::pair<uint32_t, uint32_t>, ComPtr<ID3D11SamplerState>> g_samplers;

// Statistics, logged every 10 s from renderer_present: where the GPU thread's time goes.
struct Statistics {
    uint64_t presents = 0, draws = 0, vertices = 0, vertex_ticks = 0;
    uint64_t hashed_bytes = 0, hash_ticks = 0, uploads = 0, upload_ticks = 0, draw_ticks = 0;
    // Parts of draw_ticks: target/shader lookup, vertex upload, state, textures (with state when not apart), the
    // Draw call.
    uint64_t part_ticks[5] = {};
    uint64_t swap_ticks = 0;  // in Present: long when the graphics card is behind
} g_stats;
ULONGLONG g_stats_since = 0;

uint64_t ticks() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(now.QuadPart);
}

double milliseconds(uint64_t t) {
    static const double frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    return t * 1000.0 / frequency;
}

void log_statistics() {
    const ULONGLONG now = GetTickCount64();
    if (!g_stats_since) g_stats_since = now;
    if (now - g_stats_since < 10000) return;
    const double seconds = (now - g_stats_since) / 1000.0;
    log_line("renderer: %.1f fps, %.0f draws/s, %.0f vertices/s | per second: vertices %.0f ms, texture hash %.0f ms "
             "(%.1f MB), %.0f uploads %.0f ms, D3D %.0f ms",
             g_stats.presents / seconds, g_stats.draws / seconds, g_stats.vertices / seconds,
             milliseconds(g_stats.vertex_ticks) / seconds, milliseconds(g_stats.hash_ticks) / seconds,
             g_stats.hashed_bytes / seconds / 1048576.0, g_stats.uploads / seconds,
             milliseconds(g_stats.upload_ticks) / seconds, milliseconds(g_stats.draw_ticks) / seconds);
    log_line("renderer: D3D parts per second: setup %.0f ms, vertex upload %.0f ms, state and textures %.0f ms, "
             "draw %.0f ms; Present %.0f ms",
             milliseconds(g_stats.part_ticks[0]) / seconds, milliseconds(g_stats.part_ticks[1]) / seconds,
             milliseconds(g_stats.part_ticks[2] + g_stats.part_ticks[3]) / seconds,
             milliseconds(g_stats.part_ticks[4]) / seconds, milliseconds(g_stats.swap_ticks) / seconds);
    g_stats = {};
    g_stats_since = now;
}

uint64_t hash_memory(const uint8_t* data, size_t size) {
    uint64_t hash = 1469598103934665603ull;
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t word;
        std::memcpy(&word, data + i, 8);
        hash = (hash ^ word) * 1099511628211ull;
    }
    for (; i < size; ++i) hash = (hash ^ data[i]) * 1099511628211ull;
    return hash;
}

// Only to see whether memory changed: four independent 32-bit lanes, which a 32-bit build multiplies in one
// instruction each (hash_memory's 64-bit multiply is a library call here); about three times faster.
uint64_t change_hash(const uint8_t* data, size_t size) {
    uint32_t lane[4] = {0x811C9DC5u, 0x01000193u, 0x9E3779B1u, 0x85EBCA6Bu};
    size_t i = 0;
    for (; i + 16 <= size; i += 16) {
        uint32_t words[4];
        std::memcpy(words, data + i, 16);
        for (int k = 0; k < 4; ++k) lane[k] = (lane[k] ^ words[k]) * 0x01000193u;
    }
    for (; i < size; ++i) lane[i & 3] = (lane[i & 3] ^ data[i]) * 0x01000193u;
    const uint32_t low = lane[0] ^ (lane[2] * 0x9E3779B1u), high = lane[1] ^ (lane[3] * 0x85EBCA6Bu);
    return (static_cast<uint64_t>(high) << 32 | low) ^ size;
}

ComPtr<ID3DBlob> compile(const char* source, size_t size, const char* profile, const char* name) {
    ComPtr<ID3DBlob> code, errors;
    ProfileScope scope(ProfileTime::shader_compile);
    const HRESULT hr = D3DCompile(source, size, name, nullptr, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL1,
                                  0, &code, &errors);
    if (FAILED(hr)) {
        log_line("renderer: %s failed to compile: %s", name,
                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return nullptr;
    }
    return code;
}

// Internal resolution: every render target is this many times the game's size in each direction, and drawn
// scaled (positions arrive relative to the game's size, so only viewports, scissors, clears and copies change).
// The picture is reduced to the window when presented: smoother edges and sharper detail (setting render_scale,
// read when the renderer starts). It need not be whole: 1080 lines are 2.25 times the game's 480. Texture sizes,
// scissors and copies round with scaled(), the same everywhere, so that a colour and a depth target of the same
// game size always match; viewports keep the exact value.
float g_scale = 1;

LONG scaled(uint32_t pixels) { return std::lround(static_cast<float>(pixels) * g_scale); }
uint32_t scaled_size(uint32_t pixels) { return static_cast<uint32_t>(scaled(pixels)); }
float scaled_float(uint32_t pixels) { return static_cast<float>(pixels) * g_scale; }

// The graphics card stopped answering (a driver crash or update, a GPU overloaded): nothing drawn from here on would
// show. mc3port says so and starts again, once: a second loss in a row is not one a restart mends.
[[noreturn]] void device_lost(HRESULT hr) {
    constexpr wchar_t kRestarted[] = L"MC3_RESTARTED_AFTER_GPU_LOSS";
    const HRESULT reason = g_device ? g_device->GetDeviceRemovedReason() : hr;
    log_line("renderer: the graphics card stopped responding: %08lX, reason %08lX", hr, reason);
    const bool restart = !GetEnvironmentVariableW(kRestarted, nullptr, 0);
    if (!GetEnvironmentVariableW(L"MC3_NO_DIALOG", nullptr, 0)) {
        MessageBoxA(nullptr,
                    restart ? "The graphics card stopped responding, so the game has to start again. Progress since "
                              "your last save is lost.\n\nIf it happens again, update the graphics driver or lower "
                              "the resolution in the settings."
                            : "The graphics card stopped responding again. Update the graphics driver or lower the "
                              "resolution in the settings, then start mc3port again.",
                    "mc3port", MB_ICONERROR | MB_OK);
    }
    if (restart) {
        wchar_t path[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, path, MAX_PATH)) {
            SetEnvironmentVariableW(kRestarted, L"1");
            // The new process waits for this one before it touches the log, which says why it restarted.
            SetEnvironmentVariableW(L"MC3_RESTART_FROM", std::to_wstring(GetCurrentProcessId()).c_str());
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            std::wstring command = GetCommandLineW();
            if (CreateProcessW(path, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
            } else {
                log_line("renderer: could not start again: error %lu", GetLastError());
            }
        }
    }
    end_process(1);
}

// A failed creation used to pass silently and leave a hole in the picture; the log now says what was asked for,
// since a driver may refuse a format or a size that others accept. A few lines are enough: the first ones tell.
void log_resource_failure(const char* what, HRESULT hr, const D3D11_TEXTURE2D_DESC& desc) {
    static std::atomic<int> reported{0};
    if (reported++ >= 20) return;
    log_line("renderer: cannot create %s: error %08lX (%ux%u, %u levels, %u slices, format %u, usage %u, bind %X, "
             "cpu %X, misc %X)",
             what, hr, desc.Width, desc.Height, desc.MipLevels, desc.ArraySize, static_cast<unsigned>(desc.Format),
             static_cast<unsigned>(desc.Usage), desc.BindFlags, desc.CPUAccessFlags, desc.MiscFlags);
    if (hr == DXGI_ERROR_DEVICE_REMOVED && g_device) {
        log_line("renderer: the Direct3D device was removed: %08lX", g_device->GetDeviceRemovedReason());
    }
}

void log_resource_failure(const char* what, HRESULT hr, const D3D11_BUFFER_DESC& desc) {
    static std::atomic<int> reported{0};
    if (reported++ >= 20) return;
    log_line("renderer: cannot create %s: error %08lX (%u bytes, usage %u, bind %X, cpu %X, misc %X)", what, hr,
             desc.ByteWidth, static_cast<unsigned>(desc.Usage), desc.BindFlags, desc.CPUAccessFlags, desc.MiscFlags);
}

// The surface at `address`, at least width x height. A smaller request reuses the existing target (the draw's
// viewport covers only its part); a larger one grows it and keeps what was drawn so far. Sizes are the game's;
// the texture is g_scale times larger.
Target* get_target(uint32_t address, uint32_t width, uint32_t height) {
    Target& target = g_targets[address];
    if (target.texture && target.width >= width && target.height >= height) return &target;
    width = std::max(width, target.width);
    height = std::max(height, target.height);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = scaled_size(width);
    desc.Height = scaled_size(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = target.format;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Target grown{};
    grown.format = target.format;
    if (const HRESULT hr = g_device->CreateTexture2D(&desc, nullptr, &grown.texture); FAILED(hr)) {
        log_resource_failure("a larger render target", hr, desc);
        return nullptr;
    }
    g_device->CreateRenderTargetView(grown.texture.Get(), nullptr, &grown.rtv);
    g_device->CreateShaderResourceView(grown.texture.Get(), nullptr, &grown.srv);
    grown.width = width;
    grown.height = height;
    if (target.texture) {
        const D3D11_BOX box{0, 0, 0, scaled_size(target.width), scaled_size(target.height), 1};
        g_context->CopySubresourceRegion(grown.texture.Get(), 0, 0, 0, 0, target.texture.Get(), 0, &box);
    }
    target = grown;
    log_line("renderer: render target %ux%u at %08X", width, height, address);
    return &target;
}

ComPtr<ID3D11VertexShader> g_copy_vertex_shader;
ComPtr<ID3D11PixelShader> g_copy_pixel_shader;

// Switches a render target to a float format (keeping its contents), for the Xbox's signed blend equations: on
// UNORM a subtraction below zero would clamp and lose the count.
// What bind_draw_state last gave Direct3D: a draw like the one before (most are) sets only what differs. Code that
// binds other state (effects, occlusion, presenting) forgets it on the way out (ForgetBound).
struct BoundState {
    bool valid = false;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    D3D11_VIEWPORT viewport{};
    D3D11_RECT scissor{};
    ID3D11RasterizerState* rasterizer = nullptr;
    ID3D11BlendState* blend = nullptr;
    float blend_color[4] = {};
    ID3D11DepthStencilState* depth = nullptr;
    UINT stencil_reference = 0;
    CombinerConstants constants{};
    ID3D11ShaderResourceView* views[4] = {};
    ID3D11SamplerState* samplers[4] = {};
    ID3D11PixelShader* shader = nullptr;
};
BoundState g_bound;
struct ForgetBound {
    ~ForgetBound() { g_bound.valid = false; }
};

void make_float_target(Target& target, uint32_t address) {
    const ForgetBound forget;
    if (target.format == DXGI_FORMAT_R16G16B16A16_FLOAT || !g_copy_pixel_shader) return;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = scaled_size(target.width);
    desc.Height = scaled_size(target.height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Target converted{};
    converted.format = desc.Format;
    converted.width = target.width;
    converted.height = target.height;
    if (const HRESULT hr = g_device->CreateTexture2D(&desc, nullptr, &converted.texture); FAILED(hr)) {
        log_resource_failure("a converted render target", hr, desc);
        return;
    }
    g_device->CreateRenderTargetView(converted.texture.Get(), nullptr, &converted.rtv);
    g_device->CreateShaderResourceView(converted.texture.Get(), nullptr, &converted.srv);
    // Copy the contents with a full-surface triangle (CopyResource needs the same format).
    ID3D11RenderTargetView* rtv = converted.rtv.Get();
    g_context->OMSetRenderTargets(1, &rtv, nullptr);
    g_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_context->OMSetDepthStencilState(nullptr, 0);
    const D3D11_VIEWPORT viewport{0, 0, scaled_float(target.width), scaled_float(target.height), 0, 1};
    g_context->RSSetViewports(1, &viewport);
    const D3D11_RECT all{0, 0, scaled(target.width), scaled(target.height)};
    g_context->RSSetScissorRects(1, &all);
    g_context->RSSetState(g_rasterizer.Get());
    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_copy_vertex_shader.Get(), nullptr, 0);
    g_context->PSSetShader(g_copy_pixel_shader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* source = target.srv.Get();
    g_context->PSSetShaderResources(0, 1, &source);
    g_context->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    g_context->PSSetShaderResources(0, 1, &none);
    target = converted;
    log_line("renderer: render target at %08X switched to float (signed blending)", address);
}

// Depth buffers must match their render target's size exactly in D3D11: one per address and size.
DepthTarget* get_depth_target(uint32_t address, uint32_t width, uint32_t height) {
    DepthTarget& target = g_depth_targets[(static_cast<uint64_t>(width) << 48) | (static_cast<uint64_t>(height) << 32) |
                                          address];
    if (target.texture && target.width == width && target.height == height) return &target;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = scaled_size(width);
    desc.Height = scaled_size(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R24G8_TYPELESS;  // depth-stencil view and shader view of the same texture
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    target = DepthTarget{};
    if (const HRESULT hr = g_device->CreateTexture2D(&desc, nullptr, &target.texture); FAILED(hr)) {
        log_resource_failure("a depth target", hr, desc);
        return nullptr;
    }
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    g_device->CreateDepthStencilView(target.texture.Get(), &dsv, &target.dsv);
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(target.texture.Get(), &srv, &target.srv);
    target.width = width;
    target.height = height;
    return &target;
}

// --- ambient occlusion ---
//
// Corners and contacts darkened from the depth buffer, applied to a frame's 3D scene before its HUD: at the first
// draw without depth test that follows depth-tested draws on a full-size target (once per depth clear, so each view
// of the split screen gets its own). The depth is not linear and the game's projection is unknown, but 1 - depth is
// proportional to 1/z, which varies linearly across the screen on a plane: comparing a pixel with the mean of two
// opposite neighbours gives zero on flat surfaces and a positive value in creases, relative to the pixel's own
// value whatever the scale. Large differences (an object in front of another) are left out, so outlines get no halo.

constexpr char kOcclusionPixelShader[] = R"(
cbuffer Occlusion : register(b0) { float radius; float strength; float2 unused; };
Texture2D<float> depth : register(t0);
float4 main(float4 position : SV_Position) : SV_Target {
    const int2 p = int2(position.xy);
    const float centre = 1 - depth.Load(int3(p, 0));  // proportional to 1/z
    if (centre <= 1e-5) return 1;                      // cleared to the far plane: sky
    const float angle = (((p.x & 3) * 4 + (p.y & 3)) / 16.0) * 6.2831853;
    float occlusion = 0;
    [unroll] for (int i = 0; i < 6; ++i) {
        const float a = angle + i * 0.5235988;          // 30 degrees apart over a half turn
        const float r = radius * (0.35 + 0.65 * ((i * 5 + 3) % 6) / 5.0);
        const int2 o = int2(round(float2(cos(a), sin(a)) * r));
        const float one = 1 - depth.Load(int3(p + o, 0));
        const float two = 1 - depth.Load(int3(p - o, 0));
        const float crease = ((one + two) * 0.5 - centre) / centre;  // > 0: the pixel lies behind its neighbours
        occlusion += saturate(crease * 40) * (1 - smoothstep(0.15, 0.4, crease));
    }
    const float light = 1 - strength * occlusion / 6;
    return float4(light, light, light, 1);
}
)";

struct OcclusionConstants {
    float radius, strength, unused[2];
};

ComPtr<ID3D11PixelShader> g_occlusion_pixel_shader;
ComPtr<ID3D11Buffer> g_occlusion_constants;
ComPtr<ID3D11BlendState> g_multiply_blend;  // destination colour times the source (alpha untouched)
std::atomic<bool> g_effects_enabled{true};  // F10
std::atomic<RendererOverlay> g_overlay{nullptr};  // renderer_set_overlay
uint32_t g_blur_surface = 0;  // the surface a draw read while drawing into it: MC3's motion blur buffer
std::unordered_set<uint32_t> g_shown_surfaces;  // surfaces renderer_present has shown (the frame buffers)
std::unordered_set<uint32_t> g_frame_copies;    // surfaces drawn from a shown one: the speed blur's buffer

// Per depth buffer, since its last clear (the start of a scene; presents do not line up with scenes): 3D drawn,
// occlusion applied.
struct SceneState {
    bool drawn = false;
    bool occluded = false;
};
std::unordered_map<uint32_t, SceneState> g_scenes;
uint32_t g_occlusion_scenes = 0, g_full_depth_clears = 0;  // for the log: every scene should get its pass

// Setting hud_aspect. In 16:9 MC3 draws its HUD with the pixel sizes of 4:3 on a picture 720 pixels wide that is shown
// 16:9 wide, so round things (radar, dials) come out oval. A flat draw (nv2a_kelvin.cpp) over the 3D scene on a shown
// frame that lies within the HUD is narrowed by (720 / 480) / (16 / 9) towards its side: the radar's box (the scissor
// rectangle its map is cut to, with room for the icons on its edge) towards the left, the dials hud.cpp read (each
// face, the gear, the special ability's arc, the red bar) towards the right. Nothing else: texts come in pieces, and
// narrowing the pieces near a side tore them apart (over the tutorials' films and the loading screens too, where the
// last 3D scene still counted). The scissor rectangle follows the draw; the radar's clear of the alpha does not: its
// mask then covered less than the original box, and the frame showed the previous one through the rest.
uint32_t g_picture_width = 0, g_picture_height = 0;  // the last frame shown
HudPicture g_hud_picture{};                           // and where it is in the window
constexpr uint32_t kOverscan = 48;                    // 1 / kOverscan of each side is not shown
int g_debug_hud = -1;                                 // MC3_DEBUG_HUD=<n>: the first n decisions in the log
HudBox g_radar_box{};                                 // the radar's scissor rectangle...
ULONGLONG g_radar_seen = 0;                           // ...last seen then

struct Narrowing {
    float factor = 1, anchor = 0;
    float apply(float x) const { return anchor + (x - anchor) * factor; }
};

bool inside(float left, float top, float right, float bottom, const HudBox& box, float margin) {
    return left >= box.left - margin && right <= box.right + margin && top >= box.top - margin &&
           bottom <= box.bottom + margin;
}

// The narrowing for something from `left` to `right` and `top` to `bottom` (pixels) on `surface`; none when it is
// not HUD.
Narrowing hud_narrowing(const SurfaceState& surface, float left, float right, float top, float bottom,
                        uint32_t texture) {
    Narrowing n;
    static const bool on = setting_at_start("hud_aspect") != 0;  // flat draws are also measured for others
    if (!on || left > right || !g_picture_width || !g_picture_height) return n;
    const float width = static_cast<float>(g_picture_width), height = static_cast<float>(g_picture_height);
    const ULONGLONG now = GetTickCount64();
    // The radar's map: a small scissor rectangle in the lower left.
    if (surface.clip_width <= 200 && surface.clip_height <= 200 && surface.clip_x + surface.clip_width <= width / 2 &&
        surface.clip_y >= height / 2) {
        g_radar_box = {static_cast<float>(surface.clip_x), static_cast<float>(surface.clip_y),
                       static_cast<float>(surface.clip_x + surface.clip_width),
                       static_cast<float>(surface.clip_y + surface.clip_height)};
        g_radar_seen = now;
    }
    // What can show: the part inside the scissor rectangle (the radar's map is turning tiles, cut to its box).
    left = std::max(left, static_cast<float>(surface.clip_x));
    right = std::min(right, static_cast<float>(surface.clip_x + surface.clip_width));
    top = std::max(top, static_cast<float>(surface.clip_y));
    bottom = std::min(bottom, static_cast<float>(surface.clip_y + surface.clip_height));
    if (left > right || top > bottom) return n;
    const float narrow = width / height / renderer_aspect();
    const auto scene = g_scenes.find(surface.zeta_address);
    const bool over_scene = scene != g_scenes.end() && scene->second.drawn;
    const bool shown = g_shown_surfaces.count(surface.color_address) != 0;
    const bool radar = now - g_radar_seen < 1000 &&
                       inside(left, top, right, bottom, g_radar_box, 0.35f * (g_radar_box.right - g_radar_box.left));
    const std::vector<HudBox>& dials = hud_boxes();
    const bool dial = std::any_of(dials.begin(), dials.end(),
                                  [&](const HudBox& box) { return inside(left, top, right, bottom, box, 6); });
    if (narrow < 0.99f && over_scene && shown && (radar || dial)) {
        n.anchor = radar ? 0 : width;
        n.factor = narrow;
    }
    if (g_debug_hud < 0) {
        char value[16] = {};
        g_debug_hud = GetEnvironmentVariableA("MC3_DEBUG_HUD", value, sizeof(value)) ? std::atoi(value) : 0;
    }
    if (g_debug_hud > 0) {
        --g_debug_hud;
        log_line("hud: %.0f..%.0f x %.0f..%.0f on %08X (clip %u,%u %ux%u), texture %08X: %s%s%s%s-> %.3f towards %.0f",
                 left, right, top, bottom, surface.color_address, surface.clip_x, surface.clip_y, surface.clip_width,
                 surface.clip_height, texture, over_scene ? "" : "no scene ", shown ? "" : "not shown ",
                 radar ? "radar " : "", dial ? "dial " : "", n.factor, n.anchor);
    }
    return n;
}

void apply_occlusion(Target& target, DepthTarget& depth) {
    const ForgetBound forget;
    const float strength = setting("ambient_occlusion");
    if (strength <= 0 || !g_occlusion_pixel_shader || !depth.srv || !g_effects_enabled) return;
    const OcclusionConstants constants{setting("ambient_occlusion_radius") * 14.0f * g_scale, std::min(strength, 1.0f)};
    g_context->UpdateSubresource(g_occlusion_constants.Get(), 0, nullptr, &constants, 0, 0);
    ID3D11RenderTargetView* rtv = target.rtv.Get();
    g_context->OMSetRenderTargets(1, &rtv, nullptr);  // the depth is read, not bound
    // MC3_AO_VIEW=1: the occlusion alone, in grey, instead of the scene (to tune it).
    static const bool view_only = GetEnvironmentVariableA("MC3_AO_VIEW", nullptr, 0) != 0;
    g_context->OMSetBlendState(view_only ? nullptr : g_multiply_blend.Get(), nullptr, 0xFFFFFFFF);
    g_context->OMSetDepthStencilState(nullptr, 0);
    const D3D11_VIEWPORT viewport{0, 0, scaled_float(target.width), scaled_float(target.height), 0, 1};
    g_context->RSSetViewports(1, &viewport);
    const D3D11_RECT all{0, 0, scaled(target.width), scaled(target.height)};
    g_context->RSSetScissorRects(1, &all);
    g_context->RSSetState(g_rasterizer.Get());
    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_clear_vertex_shader.Get(), nullptr, 0);
    g_context->PSSetShader(g_occlusion_pixel_shader.Get(), nullptr, 0);
    ID3D11Buffer* buffer = g_occlusion_constants.Get();
    g_context->PSSetConstantBuffers(0, 1, &buffer);
    ID3D11ShaderResourceView* source = depth.srv.Get();
    g_context->PSSetShaderResources(0, 1, &source);
    g_context->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    g_context->PSSetShaderResources(0, 1, &none);
}

D3D11_BLEND blend_factor(uint32_t gl) {
    switch (gl) {
        case 0x0000: return D3D11_BLEND_ZERO;
        case 0x0001: return D3D11_BLEND_ONE;
        case 0x0300: return D3D11_BLEND_SRC_COLOR;
        case 0x0301: return D3D11_BLEND_INV_SRC_COLOR;
        case 0x0302: return D3D11_BLEND_SRC_ALPHA;
        case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
        case 0x0304: return D3D11_BLEND_DEST_ALPHA;
        case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
        case 0x0306: return D3D11_BLEND_DEST_COLOR;
        case 0x0307: return D3D11_BLEND_INV_DEST_COLOR;
        case 0x0308: return D3D11_BLEND_SRC_ALPHA_SAT;
        case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
        case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
        default: return D3D11_BLEND_ONE;
    }
}

D3D11_BLEND alpha_blend_factor(uint32_t gl) {
    switch (gl) {  // colour factors are not allowed for alpha
        case 0x0300: return D3D11_BLEND_SRC_ALPHA;
        case 0x0301: return D3D11_BLEND_INV_SRC_ALPHA;
        case 0x0306: return D3D11_BLEND_DEST_ALPHA;
        case 0x0307: return D3D11_BLEND_INV_DEST_ALPHA;
        default: return blend_factor(gl);
    }
}

D3D11_BLEND_OP blend_operation(uint32_t gl) {
    switch (gl) {
        case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
        case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
        // Xbox "signed" equations (D3DBLENDOP_REVSUBTRACTSIGNED / ADDSIGNED): the result may go below zero and come
        // back (light volumes count faces this way). They are drawn into a float target, which does not clamp.
        case 0xF005: return D3D11_BLEND_OP_REV_SUBTRACT;
        case 0xF006: return D3D11_BLEND_OP_ADD;
        case 0x8007: return D3D11_BLEND_OP_MIN;
        case 0x8008: return D3D11_BLEND_OP_MAX;
        default: return D3D11_BLEND_OP_ADD;
    }
}

ID3D11BlendState* blend_state(const DrawState& state) {
    const auto key = std::make_tuple(state.blend_enable, state.blend_source, state.blend_destination,
                                     state.blend_equation, state.color_mask);
    ComPtr<ID3D11BlendState>& cached = g_blend_states[key];
    if (cached) return cached.Get();
    D3D11_BLEND_DESC desc{};
    D3D11_RENDER_TARGET_BLEND_DESC& rt = desc.RenderTarget[0];
    rt.BlendEnable = state.blend_enable;
    rt.SrcBlend = blend_factor(state.blend_source);
    rt.DestBlend = blend_factor(state.blend_destination);
    rt.BlendOp = blend_operation(state.blend_equation);
    rt.SrcBlendAlpha = alpha_blend_factor(state.blend_source);
    rt.DestBlendAlpha = alpha_blend_factor(state.blend_destination);
    rt.BlendOpAlpha = rt.BlendOp;
    rt.RenderTargetWriteMask = static_cast<UINT8>(((state.color_mask & 0x10000) ? D3D11_COLOR_WRITE_ENABLE_RED : 0) |
                                                  ((state.color_mask & 0x100) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0) |
                                                  ((state.color_mask & 0x1) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0) |
                                                  ((state.color_mask & 0x1000000) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0));
    g_device->CreateBlendState(&desc, &cached);
    return cached.Get();
}

D3D11_STENCIL_OP stencil_operation(uint32_t gl) {
    switch (gl) {
        case 0x0000: return D3D11_STENCIL_OP_ZERO;
        case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
        case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
        case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
        case 0x150A: return D3D11_STENCIL_OP_INVERT;
        case 0x8507: return D3D11_STENCIL_OP_INCR;
        case 0x8508: return D3D11_STENCIL_OP_DECR;
        default: return D3D11_STENCIL_OP_KEEP;  // 0x1E00
    }
}

ID3D11DepthStencilState* depth_state(const DrawState& state) {
    const auto key = std::make_tuple(state.depth_test, state.depth_function, state.depth_write, state.stencil_test,
                                     state.stencil_function, state.stencil_read_mask, state.stencil_write_mask,
                                     state.stencil_fail, state.stencil_depth_fail, state.stencil_pass);
    ComPtr<ID3D11DepthStencilState>& cached = g_depth_states[key];
    if (cached) return cached.Get();
    D3D11_DEPTH_STENCIL_DESC desc{};
    desc.DepthEnable = state.depth_test || state.depth_write;
    desc.DepthWriteMask = state.depth_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    desc.DepthFunc = state.depth_test ? static_cast<D3D11_COMPARISON_FUNC>((state.depth_function & 7) + 1)
                                      : D3D11_COMPARISON_ALWAYS;
    desc.StencilEnable = state.stencil_test;
    desc.StencilReadMask = static_cast<UINT8>(state.stencil_read_mask);
    desc.StencilWriteMask = static_cast<UINT8>(state.stencil_write_mask);
    D3D11_DEPTH_STENCILOP_DESC face{};
    face.StencilFailOp = stencil_operation(state.stencil_fail);
    face.StencilDepthFailOp = stencil_operation(state.stencil_depth_fail);
    face.StencilPassOp = stencil_operation(state.stencil_pass);
    face.StencilFunc = static_cast<D3D11_COMPARISON_FUNC>((state.stencil_function & 7) + 1);
    desc.FrontFace = desc.BackFace = face;
    g_device->CreateDepthStencilState(&desc, &cached);
    return cached.Get();
}

D3D11_TEXTURE_ADDRESS_MODE address_mode(uint32_t nv) {
    switch (nv) {
        case 1: return D3D11_TEXTURE_ADDRESS_WRAP;
        case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
        case 4: return D3D11_TEXTURE_ADDRESS_BORDER;
        default: return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
}

ID3D11SamplerState* sampler(const TextureStage& stage) {
    ComPtr<ID3D11SamplerState>& cached = g_samplers[{stage.address_mode, stage.filter}];
    if (cached) return cached.Get();
    D3D11_SAMPLER_DESC desc{};
    // NV097_SET_TEXTURE_FILTER: magnification 1 nearest, 2 linear; minification 1 nearest, 2 linear, 3-6 the
    // four mipmap combinations (nearest/linear within a level, nearest/linear between levels), 7 convolution.
    const uint32_t min = (stage.filter >> 16) & 0xFF;
    const bool linear_mag = ((stage.filter >> 24) & 0xF) != 1;
    const bool linear_min = min == 2 || min == 4 || min == 6 || min == 7;
    const bool mipmaps = min >= 3 && min <= 6;
    const bool linear_mip = min == 5 || min == 6;
    static constexpr D3D11_FILTER kFilters[8] = {
        D3D11_FILTER_MIN_MAG_MIP_POINT,          D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR,
        D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT, D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR,
        D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT,   D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR,
        D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT,   D3D11_FILTER_MIN_MAG_MIP_LINEAR};
    desc.Filter = kFilters[(linear_min ? 4 : 0) | (linear_mag ? 2 : 0) | (linear_mip ? 1 : 0)];
    // Smooth (trilinear) filtering becomes anisotropic: textures seen at a slant (roads, walls) stay sharp
    // (setting anisotropy, 1..16, 1 = off).
    static const UINT anisotropy = std::clamp<UINT>(static_cast<UINT>(setting_at_start("anisotropy")), 1, 16);
    if (linear_min && linear_mag && mipmaps && anisotropy > 1) {
        desc.Filter = D3D11_FILTER_ANISOTROPIC;
        desc.MaxAnisotropy = anisotropy;
    }
    desc.AddressU = address_mode(stage.address_mode & 0xF);
    desc.AddressV = address_mode((stage.address_mode >> 8) & 0xF);
    desc.AddressW = address_mode((stage.address_mode >> 16) & 0xF);
    desc.MaxLOD = mipmaps ? D3D11_FLOAT32_MAX : 0;  // without mipmap filtering only the top level is used
    g_device->CreateSamplerState(&desc, &cached);
    return cached.Get();
}

// Current write-tracking generation, refreshed at most every 2 ms: a write that lands just after a refresh is
// picked up by the next one.
uint32_t g_write_generation = 0;
uint64_t g_write_refreshed_at = 0;

void refresh_write_tracking() {
    static const uint64_t interval = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<uint64_t>(f.QuadPart) / 500;
    }();
    const uint64_t now = ticks();
    if (g_write_generation && now - g_write_refreshed_at < interval) return;
    g_write_generation = refresh_physical_writes();
    g_write_refreshed_at = now;
}

// textures_replace\<hash>.png (or .jpg) in the data folder: a picture drawn instead of the game's texture whose
// bytes have that hash (16 hex digits, as MC3_DUMP_2D and MC3_DUMP_TEXTURE name their files), of any size. For
// pictures made from one's own copy of the game, sharper or redrawn; mc3port ships none. Linear textures (addressed
// in texels) are left as they are.
std::unordered_map<uint64_t, std::filesystem::path> g_replacements;

void find_replacements() {
    const std::filesystem::path folder = data_directory() / "textures_replace";
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        const std::wstring extension = entry.path().extension().wstring();
        if (extension != L".png" && extension != L".jpg" && extension != L".PNG" && extension != L".JPG") continue;
        const std::wstring stem = entry.path().stem().wstring();
        if (stem.size() != 16) continue;
        wchar_t* end = nullptr;
        const uint64_t hash = std::wcstoull(stem.c_str(), &end, 16);
        if (end == stem.c_str() + 16) g_replacements[hash] = entry.path();
    }
    if (!g_replacements.empty()) log_line("renderer: %zu replacement textures", g_replacements.size());
}

// The picture `file` as BGRA, through Windows' image decoders (PNG, JPEG).
bool load_picture(const std::filesystem::path& file, std::vector<uint8_t>& bgra, UINT& width, UINT& height) {
    thread_local const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // the launcher's thread and the GPU's
    (void)com;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                  &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&width, &height)) || !width || !height || width > 8192 || height > 8192) {
        return false;
    }
    bgra.resize(static_cast<size_t>(width) * height * 4);
    return SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(bgra.size()), bgra.data()));
}

// The game's atlas of button symbols shows White and Black as plain discs. Here those buttons are LB and RB
// (input_hle.cpp), so the discs get those letters, drawn in code as the texture is decoded.
constexpr uint64_t kButtonAtlas = 0xA30CEDEE3C0953C1ull;

void letter_shoulder_buttons(TextureImage& atlas) {
    if (atlas.layout != TextureLayout::Bgra8 || atlas.width != 128 || atlas.height != 128) return;
    static constexpr uint8_t kLetters[3][5] = {{4, 4, 4, 4, 7}, {6, 5, 6, 5, 6}, {6, 5, 6, 5, 5}};  // L, B, R; 3 x 5
    const auto letter = [&](int which, int left, int top, uint8_t shade) {
        for (int y = 0; y < 10; ++y) {
            for (int x = 0; x < 6; ++x) {
                if (!(kLetters[which][y / 2] >> (2 - x / 2) & 1)) continue;
                uint8_t* pixel = atlas.data.data() + static_cast<size_t>(top + y) * atlas.row_pitch + (left + x) * 4;
                pixel[0] = pixel[1] = pixel[2] = shade;
                pixel[3] = 255;
            }
        }
    };
    letter(0, 87, 71, 24), letter(1, 95, 71, 24);    // the white disc, centred at 94,76: LB, dark
    letter(2, 14, 107, 240), letter(1, 22, 107, 240);  // the black disc, centred at 21,112: RB, light
}

// A replacement picture as a texture with its mip levels made by the graphics card.
bool make_replacement(const std::filesystem::path& file, CachedTexture& cached) {
    std::vector<uint8_t> bgra;
    UINT width = 0, height = 0;
    if (!load_picture(file, bgra, width, height)) {
        log_line("renderer: cannot read the replacement texture %s", file.filename().string().c_str());
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 0;  // all of them
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &texture)) ||
        FAILED(g_device->CreateShaderResourceView(texture.Get(), nullptr, &srv))) {
        log_resource_failure("a replacement texture", E_FAIL, desc);
        return false;
    }
    g_context->UpdateSubresource(texture.Get(), 0, nullptr, bgra.data(), width * 4, 0);
    g_context->GenerateMips(srv.Get());
    cached.texture = texture;
    cached.srv = srv;
    return true;
}

// `flat`: the draw is flat over the 3D scene (the HUD): with sharp_2d its textures and the cars' liveries are enlarged
// (texture_upscale.cpp); not the linear ones, which are addressed in texels.
ID3D11ShaderResourceView* texture_view(const TextureStage& stage, bool flat) {
    const TextureDescriptor& d = stage.descriptor;
    static const bool sharp_2d = setting_at_start("sharp_2d") != 0;
    // A car's livery is one 256x256 paletted picture stretched over the whole car, the only texture of that kind seen
    // in the city: close up, at more than 480 lines, its texels show as steps along the stripes.
    const bool livery = (d.format >> 8 & 0x7F) == 0x0B && (d.format >> 16 & 0xFFF) == 0x881;
    const bool enlarge = (flat || livery) && sharp_2d && !texture_is_linear(d);
    // A texture that is a render target (render to texture) is sampled from the GPU copy.
    const auto target = g_targets.find(d.address);
    if (target != g_targets.end()) return target->second.srv.Get();

    uint32_t width = 0, height = 0, bytes = 0;
    if (!texture_extent(d, &width, &height, &bytes)) return nullptr;
    // The game's own mip levels follow the top one; they are checked and hashed with it.
    uint32_t levels = texture_levels(d);
    if (levels > 1) {
        const uint32_t all = texture_total_bytes(d);
        if (physical_pointer(d.address + all - 1)) {
            bytes = all;
        } else {
            levels = 1;
        }
    }
    const uint8_t* memory = physical_pointer(d.address);
    if (!memory || !physical_pointer(d.address + bytes - 1)) return nullptr;
    const TextureKey key = std::make_tuple(d.address, d.format, d.pitch, d.rect, d.palette, enlarge);
    CachedTexture& cached = g_textures[key];
    std::vector<TextureImage> larger;
    if (cached.waiting && g_enlarger.take(key, cached.content_hash, larger)) {
        cached.waiting = false;
        if (!larger.empty()) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = larger[0].width;
            desc.Height = larger[0].height;
            desc.MipLevels = static_cast<UINT>(larger.size());
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            std::vector<D3D11_SUBRESOURCE_DATA> data;
            for (const TextureImage& level : larger) data.push_back({level.data.data(), level.row_pitch, 0});
            ComPtr<ID3D11Texture2D> texture;
            ComPtr<ID3D11ShaderResourceView> srv;
            if (SUCCEEDED(g_device->CreateTexture2D(&desc, data.data(), &texture)) &&
                SUCCEEDED(g_device->CreateShaderResourceView(texture.Get(), nullptr, &srv))) {
                cached.texture = texture;
                cached.srv = srv;
            } else {
                log_resource_failure("an enlarged 2D texture", E_FAIL, desc);
            }
        }
    }
    // Unchanged memory needs no look at all; written memory is hashed, and decoded again only if it differs.
    uint32_t written = physical_write_generation(d.address, bytes);
    if (d.palette) written = std::max(written, physical_write_generation(d.palette, 1024));
    if (cached.srv && written <= cached.checked_generation) return cached.srv.Get();

    const uint64_t hash_start = ticks();
    // The texture's own bytes name its replacement (the palette address is set for every format, and its memory
    // changes between runs).
    const auto hash_piece = [](const uint8_t* data, uint32_t size) {
        g_stats.hashed_bytes += size;
        return change_hash(data, size);
    };
    const uint64_t hash = hash_written_pages(cached.page_hashes, memory, d.address & 0x7FFFFFFF, bytes, cached.checked_generation,
                                             [](uint32_t address) { return physical_write_generation(address, 1); }, hash_piece) ^
                          (d.palette ? hash_piece(physical_pointer(d.palette), 1024) : 0);
    g_stats.hash_ticks += ticks() - hash_start;
    cached.checked_generation = g_write_generation;
    if (cached.srv && cached.content_hash == hash) return cached.srv.Get();
    static const bool replacements = (find_replacements(), !g_replacements.empty());
    // The name of replacement and dumped pictures: hash_memory of the texture's own bytes, as in earlier versions.
    static const bool dumping = GetEnvironmentVariableA("MC3_DUMP_TEXTURE", nullptr, 0) != 0 ||
                                GetEnvironmentVariableA("MC3_DUMP_2D", nullptr, 0) != 0;
    const bool maybe_atlas = width == 128 && height == 128;
    const uint64_t own_hash = replacements || dumping || maybe_atlas ? hash_memory(memory, bytes) : 0;
    if (replacements && !texture_is_linear(d)) {
        const auto replacement = g_replacements.find(own_hash);
        if (replacement != g_replacements.end()) {
            cached = CachedTexture{};
            if (make_replacement(replacement->second, cached)) {
                cached.content_hash = hash;
                log_line("renderer: texture %08X replaced by %s", d.address,
                         replacement->second.filename().string().c_str());
                return cached.srv.Get();
            }
        }
    }

    const uint64_t upload_start = ticks();
    ProfileScope upload_scope(ProfileTime::texture_upload);
    struct Account {
        uint64_t start;
        ~Account() {
            g_stats.upload_ticks += ticks() - start;
            ++g_stats.uploads;
        }
    } account{upload_start};
    TextureImage image = decode_texture(d);
    if (image.layout == TextureLayout::Unsupported) return nullptr;
    const bool button_atlas = own_hash == kButtonAtlas;
    if (button_atlas) {
        letter_shoulder_buttons(image);
        log_line("renderer: LB and RB drawn on the button symbols at %08X", d.address);
    }
    // MC3_DUMP_TEXTURE=<hex address>: save the textures in that 4 KB page once decoded (BGRA with alpha) as
    // texture-<address>-<hash>.bmp (resources move a little between runs; the hash names a replacement).
    static const uint32_t dump_texture = [] {
        char value[16] = {};
        return GetEnvironmentVariableA("MC3_DUMP_TEXTURE", value, sizeof(value)) ? std::strtoul(value, nullptr, 16) : 0u;
    }();
    // MC3_DUMP_2D=1: also every texture of the HUD's flat draws (those sharp_2d enlarges). Block-compressed ones as
    // they are, in a DDS file (texture-<address>-<hash>.dds).
    static const bool dump_2d = GetEnvironmentVariableA("MC3_DUMP_2D", nullptr, 0) != 0;
    const bool dump = (dump_texture && (d.address & ~0xFFFu) == (dump_texture & ~0xFFFu)) || (dump_2d && flat);
    if (dump && image.layout != TextureLayout::Bgra8) {
        char name[64];
        std::snprintf(name, sizeof(name), "texture-%08X-%016llX.dds", d.address,
                      static_cast<unsigned long long>(own_hash));
        if (FILE* file = std::fopen(name, "wb")) {
            uint32_t header[32] = {0x20534444, 124, 0x1007, image.height, image.width, image.row_pitch, 0, 1};
            header[19] = 32;      // pixel format size
            header[20] = 4;       // DDPF_FOURCC
            header[21] = image.layout == TextureLayout::Bc1   ? 0x31545844   // "DXT1"
                         : image.layout == TextureLayout::Bc2 ? 0x33545844   // "DXT3"
                                                              : 0x35545844;  // "DXT5"
            header[27] = 0x1000;  // DDSCAPS_TEXTURE
            std::fwrite(header, sizeof(header), 1, file);
            std::fwrite(image.data.data(), 1, image.data.size(), file);
            std::fclose(file);
        }
    }
    if (dump && image.layout == TextureLayout::Bgra8) {
        char name[64];
        std::snprintf(name, sizeof(name), "texture-%08X-%016llX.bmp", d.address,
                      static_cast<unsigned long long>(own_hash));
        if (FILE* file = std::fopen(name, "wb")) {
            BITMAPFILEHEADER header{};
            BITMAPINFOHEADER info{};
            header.bfType = 0x4D42;
            header.bfOffBits = sizeof(header) + sizeof(info);
            header.bfSize = header.bfOffBits + image.width * 4 * image.height;
            info.biSize = sizeof(info);
            info.biWidth = static_cast<LONG>(image.width);
            info.biHeight = -static_cast<LONG>(image.height);
            info.biPlanes = 1;
            info.biBitCount = 32;
            std::fwrite(&header, sizeof(header), 1, file);
            std::fwrite(&info, sizeof(info), 1, file);
            std::fwrite(image.data.data(), 1, image.data.size(), file);
            std::fclose(file);
        }
    }
    std::vector<TextureImage> lower_levels;
    for (uint32_t level = 1; level < levels && !button_atlas; ++level) {  // the atlas's own would keep the plain discs
        lower_levels.push_back(decode_texture(texture_level(d, level)));
        if (lower_levels.back().layout != image.layout) {
            lower_levels.clear();
            break;
        }
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = image.width;
    desc.Height = image.height;
    desc.MipLevels = 1 + static_cast<UINT>(lower_levels.size());
    desc.ArraySize = 1;
    desc.Format = image.layout == TextureLayout::Bc1   ? DXGI_FORMAT_BC1_UNORM
                  : image.layout == TextureLayout::Bc2 ? DXGI_FORMAT_BC2_UNORM
                  : image.layout == TextureLayout::Bc3 ? DXGI_FORMAT_BC3_UNORM
                                                       : DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<D3D11_SUBRESOURCE_DATA> data{{image.data.data(), image.row_pitch, 0}};
    for (const TextureImage& level : lower_levels) data.push_back({level.data.data(), level.row_pitch, 0});
    cached = CachedTexture{};
    if (const HRESULT hr = g_device->CreateTexture2D(&desc, data.data(), &cached.texture); FAILED(hr)) {
        log_resource_failure("a texture", hr, desc);
        return nullptr;
    }
    g_device->CreateShaderResourceView(cached.texture.Get(), nullptr, &cached.srv);
    cached.content_hash = hash;
    if (enlarge) {
        cached.waiting = true;
        g_enlarger.request(key, hash, image);
    }
    return cached.srv.Get();
}

// Named after the shader's text: any change of it is a new file.
std::filesystem::path shader_cache_file(const char* prefix, const std::string& text) {
    uint64_t hash = 14695981039346656037ull;
    for (const char ch : text) hash = (hash ^ static_cast<uint8_t>(ch)) * 1099511628211ull;
    char file_name[40];
    std::snprintf(file_name, sizeof(file_name), "%s-%016llX.cso", prefix, static_cast<unsigned long long>(hash));
    return data_directory() / "shader_cache" / file_name;
}

ID3D11PixelShader* pixel_shader(const CombinerProgram& program) {
    const uint64_t key = combiner_hash(program);
    const auto it = g_pixel_shaders.find(key);
    if (it != g_pixel_shaders.end()) return it->second.Get();
    ComPtr<ID3D11PixelShader>& shader = g_pixel_shaders[key];
    const std::string source = generate_pixel_shader(program);
    // Compiled here, on the GPU thread: a quarter of a second on entering a city. From the second run on, read from disk.
    const std::filesystem::path file = shader_cache_file("ps", source);
    std::ifstream cached(file, std::ios::binary);
    const std::vector<char> stored((std::istreambuf_iterator<char>(cached)), std::istreambuf_iterator<char>());
    if (!stored.empty() && SUCCEEDED(g_device->CreatePixelShader(stored.data(), stored.size(), nullptr, &shader))) {
        return shader.Get();
    }
    if (ComPtr<ID3DBlob> code = compile(source.c_str(), source.size(), "ps_5_0", "combiner")) {
        if (SUCCEEDED(g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader))) {
            std::error_code error;
            std::filesystem::create_directories(file.parent_path(), error);
            std::ofstream(file, std::ios::binary)
                .write(static_cast<const char*>(code->GetBufferPointer()), code->GetBufferSize());
        }
    } else {
        log_line("%s", source.c_str());
    }
    return shader.Get();
}

// MC3_DUMP_FRAMES=<n>: every n-th presented frame is read back and saved as frame-<count>.bmp next to the log,
// to check the rendering without looking at the screen.
uint32_t g_dump_every = 0;
uint64_t g_presented = 0;

// MC3_DUMP_TARGET=<hex address>: with each dumped frame, also the render target at that physical address.
uint32_t g_dump_target = 0;

void dump_frame(ID3D11Texture2D* texture, const char* prefix = "frame") {
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (const HRESULT hr = g_device->CreateTexture2D(&desc, nullptr, &staging); FAILED(hr)) {
        log_resource_failure("a staging copy", hr, desc);
        return;
    }
    g_context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(g_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return;
    char name[64];
    std::snprintf(name, sizeof(name), "%s-%05llu.bmp", prefix, static_cast<unsigned long long>(g_presented));
    if (FILE* file = std::fopen(name, "wb")) {
        const uint32_t row = desc.Width * 4;
        BITMAPFILEHEADER header{};
        BITMAPINFOHEADER info{};
        header.bfType = 0x4D42;
        header.bfOffBits = sizeof(header) + sizeof(info);
        header.bfSize = header.bfOffBits + row * desc.Height;
        info.biSize = sizeof(info);
        info.biWidth = static_cast<LONG>(desc.Width);
        info.biHeight = -static_cast<LONG>(desc.Height);
        info.biPlanes = 1;
        info.biBitCount = 32;
        std::fwrite(&header, sizeof(header), 1, file);
        std::fwrite(&info, sizeof(info), 1, file);
        for (uint32_t y = 0; y < desc.Height; ++y) {
            std::fwrite(static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch, row, 1, file);
        }
        std::fclose(file);
        log_line("renderer: saved %s", name);
    }
    g_context->Unmap(staging.Get(), 0);
}

// The swap chain follows the window's client area (it changes with resizing and full screen).
void ensure_swap_chain_size() {
    const ForgetBound forget;
    RECT client{};
    GetClientRect(g_window_handle, &client);
    const auto width = static_cast<uint32_t>(std::max<LONG>(1, client.right - client.left));
    const auto height = static_cast<uint32_t>(std::max<LONG>(1, client.bottom - client.top));
    if (width == g_swap_width && height == g_swap_height && g_final_rtv) return;
    std::scoped_lock lock(g_swap_mutex, g_present_mutex);
    g_context->OMSetRenderTargets(0, nullptr, nullptr);
    g_final_rtv.Reset();
    g_final_texture.Reset();
    g_back_buffer.Reset();
    g_swap_chain->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    g_swap_width = width;
    g_swap_height = height;
    g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&g_back_buffer));
    D3D11_TEXTURE2D_DESC desc{};
    g_back_buffer->GetDesc(&desc);
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = 0;
    g_device->CreateTexture2D(&desc, nullptr, &g_final_texture);
    g_device->CreateRenderTargetView(g_final_texture.Get(), nullptr, &g_final_rtv);
}

// --- modern effects, applied to the finished frame when it is presented ---
//
// Bloom: the bright parts at a quarter of the frame's size, blurred (horizontal then vertical Gaussian) and added.
// The final pass then sharpens (contrast-adaptive, like AMD CAS), adds the bloom, sets contrast and saturation and
// darkens the corners a little. Settings in mc3port.ini; F10 switches the effects off and on to compare.

struct PostConstants {
    float source_texel[2];  // 1 / size of the texture read
    float output_texel[2];  // 1 / size of the frame's rectangle in the window
    float sharpen, bloom, bloom_threshold, saturation;
    float contrast, vignette, direction_x, direction_y;  // blur direction (in source texels)
};

ComPtr<ID3D11Buffer> g_post_constants;
ComPtr<ID3D11PixelShader> g_bright_pixel_shader, g_blur_pixel_shader, g_final_pixel_shader;
ComPtr<ID3D11Texture2D> g_bloom_texture[2];
ComPtr<ID3D11RenderTargetView> g_bloom_rtv[2];
ComPtr<ID3D11ShaderResourceView> g_bloom_srv[2];
uint32_t g_bloom_width = 0, g_bloom_height = 0;

constexpr char kBrightPixelShader[] = R"(
cbuffer Post : register(b0) { float2 source_texel; float2 output_texel; float sharpen, bloom, threshold, saturation;
                              float contrast, vignette; float2 direction; };
Texture2D frame : register(t0);
SamplerState filtered : register(s0);
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    // A quarter-size texel covers 4x4 frame texels: four bilinear taps average them.
    float3 sum = 0;
    [unroll] for (int i = 0; i < 4; ++i) {
        const float2 offset = float2((i & 1) ? 1 : -1, (i & 2) ? 1 : -1) * source_texel;
        sum += frame.SampleLevel(filtered, uv + offset, 0).rgb;
    }
    const float3 colour = sum / 4;
    const float brightness = max(colour.r, max(colour.g, colour.b));
    const float weight = saturate((brightness - threshold) / max(1 - threshold, 0.01));
    return float4(colour * weight, 1);
}
)";

constexpr char kBlurPixelShader[] = R"(
cbuffer Post : register(b0) { float2 source_texel; float2 output_texel; float sharpen, bloom, threshold, saturation;
                              float contrast, vignette; float2 direction; };
Texture2D source : register(t0);
SamplerState filtered : register(s0);
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    // 9-tap Gaussian folded into 5 bilinear taps.
    const float weights[3] = {0.2270270270, 0.3162162162, 0.0702702703};
    const float offsets[3] = {0.0, 1.3846153846, 3.2307692308};
    const float2 step = direction * source_texel;
    float3 sum = source.SampleLevel(filtered, uv, 0).rgb * weights[0];
    [unroll] for (int i = 1; i < 3; ++i) {
        sum += source.SampleLevel(filtered, uv + step * offsets[i], 0).rgb * weights[i];
        sum += source.SampleLevel(filtered, uv - step * offsets[i], 0).rgb * weights[i];
    }
    return float4(sum, 1);
}
)";

constexpr char kFinalPixelShader[] = R"(
cbuffer Post : register(b0) { float2 source_texel; float2 output_texel; float sharpen, bloom, threshold, saturation;
                              float contrast, vignette; float2 direction; };
Texture2D frame : register(t0);
Texture2D glow : register(t1);
SamplerState filtered : register(s0);
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float3 c = frame.SampleLevel(filtered, uv, 0).rgb;
    if (sharpen > 0) {
        // Contrast-adaptive sharpening on the cross of neighbouring output pixels.
        const float3 n = frame.SampleLevel(filtered, uv + float2(0, -output_texel.y), 0).rgb;
        const float3 s = frame.SampleLevel(filtered, uv + float2(0, output_texel.y), 0).rgb;
        const float3 w = frame.SampleLevel(filtered, uv + float2(-output_texel.x, 0), 0).rgb;
        const float3 e = frame.SampleLevel(filtered, uv + float2(output_texel.x, 0), 0).rgb;
        const float3 low = min(c, min(min(n, s), min(w, e)));
        const float3 high = max(c, max(max(n, s), max(w, e)));
        const float3 amplitude = sqrt(saturate(min(low, 1 - high) / max(high, 1e-4)));
        const float3 peak = -1 / lerp(8.0, 5.0, saturate(sharpen));
        const float3 weight = amplitude * peak;
        c = saturate((c + (n + s + w + e) * weight) / (1 + 4 * weight));
    }
    c += glow.SampleLevel(filtered, uv, 0).rgb * bloom;
    c = (c - 0.5) * contrast + 0.5;
    const float luma = dot(c, float3(0.299, 0.587, 0.114));
    c = lerp(luma.xxx, c, saturation);
    const float2 centred = uv - 0.5;
    c *= 1 - vignette * smoothstep(0.2, 0.75, dot(centred, centred) * 2);
    return float4(saturate(c), 1);
}
)";

void ensure_bloom_targets(uint32_t width, uint32_t height) {
    const ForgetBound forget;
    width = std::max(1u, width / 4), height = std::max(1u, height / 4);
    if (g_bloom_texture[0] && g_bloom_width == width && g_bloom_height == height) return;
    for (int i = 0; i < 2; ++i) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        g_bloom_texture[i].Reset();
        g_bloom_rtv[i].Reset();
        g_bloom_srv[i].Reset();
        g_device->CreateTexture2D(&desc, nullptr, &g_bloom_texture[i]);
        g_device->CreateRenderTargetView(g_bloom_texture[i].Get(), nullptr, &g_bloom_rtv[i]);
        g_device->CreateShaderResourceView(g_bloom_texture[i].Get(), nullptr, &g_bloom_srv[i]);
    }
    g_bloom_width = width;
    g_bloom_height = height;
}

// One full-surface pass into `rtv` (viewport `area`) reading `sources`.
void post_pass(ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& area, ID3D11PixelShader* shader,
               ID3D11ShaderResourceView* const* sources, UINT source_count, const PostConstants& constants) {
    const ForgetBound forget;
    g_context->UpdateSubresource(g_post_constants.Get(), 0, nullptr, &constants, 0, 0);
    g_context->OMSetRenderTargets(1, &rtv, nullptr);
    g_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_context->OMSetDepthStencilState(nullptr, 0);
    g_context->RSSetViewports(1, &area);
    const D3D11_RECT scissor{static_cast<LONG>(area.TopLeftX), static_cast<LONG>(area.TopLeftY),
                             static_cast<LONG>(area.TopLeftX + area.Width), static_cast<LONG>(area.TopLeftY + area.Height)};
    g_context->RSSetScissorRects(1, &scissor);
    g_context->RSSetState(g_rasterizer.Get());
    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_present_vertex_shader.Get(), nullptr, 0);
    g_context->PSSetShader(shader, nullptr, 0);
    ID3D11Buffer* buffer = g_post_constants.Get();
    g_context->PSSetConstantBuffers(0, 1, &buffer);
    g_context->PSSetShaderResources(0, source_count, sources);
    ID3D11SamplerState* sampler = g_present_sampler.Get();
    g_context->PSSetSamplers(0, 1, &sampler);
    g_context->Draw(3, 0);
    ID3D11ShaderResourceView* none[2] = {};
    g_context->PSSetShaderResources(0, 2, none);
}

// Draws the frame into the window: the largest rectangle of the picture's shape (renderer_aspect) that fits,
// centred, filtered, with the effects.
void present_frame(ID3D11ShaderResourceView* frame, uint32_t frame_width, uint32_t frame_height) {
    const ForgetBound forget;
    if (!g_final_rtv || !g_present_pixel_shader) return;
    if (g_effects_enabled && g_final_pixel_shader && frame_width && frame_height) {
        const float aspect = renderer_aspect();
        float width = static_cast<float>(g_swap_width), height = static_cast<float>(g_swap_height);
        if (width > height * aspect) {
            width = height * aspect;
        } else {
            height = width / aspect;
        }
        const D3D11_VIEWPORT area{std::floor((g_swap_width - width) / 2), std::floor((g_swap_height - height) / 2),
                                  width, height, 0, 1};
        PostConstants constants{};
        constants.sharpen = setting("sharpen");
        constants.bloom = setting("bloom");
        constants.bloom_threshold = setting("bloom_threshold");
        constants.saturation = setting("saturation");
        constants.contrast = setting("contrast");
        constants.vignette = setting("vignette");
        ID3D11ShaderResourceView* glow = nullptr;
        if (constants.bloom > 0) {
            ensure_bloom_targets(frame_width, frame_height);
            const D3D11_VIEWPORT quarter{0, 0, static_cast<float>(g_bloom_width), static_cast<float>(g_bloom_height), 0, 1};
            constants.source_texel[0] = 1.0f / frame_width, constants.source_texel[1] = 1.0f / frame_height;
            post_pass(g_bloom_rtv[0].Get(), quarter, g_bright_pixel_shader.Get(), &frame, 1, constants);
            constants.source_texel[0] = 1.0f / g_bloom_width, constants.source_texel[1] = 1.0f / g_bloom_height;
            constants.direction_x = 1, constants.direction_y = 0;
            ID3D11ShaderResourceView* first = g_bloom_srv[0].Get();
            post_pass(g_bloom_rtv[1].Get(), quarter, g_blur_pixel_shader.Get(), &first, 1, constants);
            constants.direction_x = 0, constants.direction_y = 1;
            ID3D11ShaderResourceView* second = g_bloom_srv[1].Get();
            post_pass(g_bloom_rtv[0].Get(), quarter, g_blur_pixel_shader.Get(), &second, 1, constants);
            glow = g_bloom_srv[0].Get();
        }
        const float black[4] = {0, 0, 0, 1};
        g_context->ClearRenderTargetView(g_final_rtv.Get(), black);
        constants.output_texel[0] = 1.0f / width, constants.output_texel[1] = 1.0f / height;
        ID3D11ShaderResourceView* sources[2] = {frame, glow};
        post_pass(g_final_rtv.Get(), area, g_final_pixel_shader.Get(), sources, 2, constants);
        return;
    }
    const float aspect = renderer_aspect();
    float width = static_cast<float>(g_swap_width), height = static_cast<float>(g_swap_height);
    if (width > height * aspect) {
        width = height * aspect;
    } else {
        height = width / aspect;
    }
    const float left = std::floor((g_swap_width - width) / 2), top = std::floor((g_swap_height - height) / 2);
    const float black[4] = {0, 0, 0, 1};
    g_context->ClearRenderTargetView(g_final_rtv.Get(), black);
    ID3D11RenderTargetView* rtv = g_final_rtv.Get();
    g_context->OMSetRenderTargets(1, &rtv, nullptr);
    g_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_context->OMSetDepthStencilState(nullptr, 0);
    const D3D11_VIEWPORT viewport{left, top, width, height, 0, 1};
    g_context->RSSetViewports(1, &viewport);
    const D3D11_RECT scissor{0, 0, static_cast<LONG>(g_swap_width), static_cast<LONG>(g_swap_height)};
    g_context->RSSetScissorRects(1, &scissor);
    g_context->RSSetState(g_rasterizer.Get());
    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_present_vertex_shader.Get(), nullptr, 0);
    g_context->PSSetShader(g_present_pixel_shader.Get(), nullptr, 0);
    g_context->PSSetShaderResources(0, 1, &frame);
    ID3D11SamplerState* sampler = g_present_sampler.Get();
    g_context->PSSetSamplers(0, 1, &sampler);
    g_context->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    g_context->PSSetShaderResources(0, 1, &none);
}

// Guest pixels (32-bit) into a region of an enlarged render target: through a small texture drawn g_scale times
// larger, since a copy cannot stretch.
ComPtr<ID3D11PixelShader> g_stretch_pixel_shader;
ComPtr<ID3D11SamplerState> g_point_sampler;
ComPtr<ID3D11Texture2D> g_stretch_source;
ComPtr<ID3D11ShaderResourceView> g_stretch_source_srv;
uint32_t g_stretch_width = 0, g_stretch_height = 0;

void upload_scaled(Target& to, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const uint8_t* pixels,
                   uint32_t pitch) {
    if (!pixels || !g_stretch_pixel_shader) return;
    if (!g_stretch_source || g_stretch_width != width || g_stretch_height != height) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        g_stretch_source.Reset();
        g_stretch_source_srv.Reset();
        if (const HRESULT hr = g_device->CreateTexture2D(&desc, nullptr, &g_stretch_source); FAILED(hr)) {
            log_resource_failure("the stretch source", hr, desc);
            return;
        }
        g_device->CreateShaderResourceView(g_stretch_source.Get(), nullptr, &g_stretch_source_srv);
        g_stretch_width = width;
        g_stretch_height = height;
    }
    g_context->UpdateSubresource(g_stretch_source.Get(), 0, nullptr, pixels, pitch, 0);
    ID3D11RenderTargetView* rtv = to.rtv.Get();
    g_context->OMSetRenderTargets(1, &rtv, nullptr);
    g_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_context->OMSetDepthStencilState(nullptr, 0);
    const D3D11_VIEWPORT viewport{scaled_float(x), scaled_float(y), scaled_float(width), scaled_float(height), 0, 1};
    g_context->RSSetViewports(1, &viewport);
    const D3D11_RECT scissor{scaled(x), scaled(y), scaled(x + width), scaled(y + height)};
    g_context->RSSetScissorRects(1, &scissor);
    g_context->RSSetState(g_rasterizer.Get());
    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_present_vertex_shader.Get(), nullptr, 0);
    g_context->PSSetShader(g_stretch_pixel_shader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* source = g_stretch_source_srv.Get();
    g_context->PSSetShaderResources(0, 1, &source);
    ID3D11SamplerState* sampler = g_point_sampler.Get();
    g_context->PSSetSamplers(0, 1, &sampler);
    g_context->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    g_context->PSSetShaderResources(0, 1, &none);
}

// Copies the finished picture into the swap chain and presents it: on the present thread, or on the GPU thread when
// the context has no multithread protection.
void present_now() {
    std::lock_guard<std::mutex> swap(g_swap_mutex);
    {
        std::lock_guard<std::mutex> drawing(g_present_mutex);
        if (!g_back_buffer || !g_final_texture) return;
        g_context->CopyResource(g_back_buffer.Get(), g_final_texture.Get());
    }
    const uint64_t start = ticks();
    const HRESULT presented = g_swap_chain->Present(0, 0);
    if (presented == DXGI_ERROR_DEVICE_REMOVED || presented == DXGI_ERROR_DEVICE_RESET) device_lost(presented);
    const uint64_t taken = ticks() - start;
    {
        std::lock_guard<std::mutex> drawing(g_present_mutex);  // read by log_statistics on the GPU thread
        g_stats.swap_ticks += taken;
    }
    static int slow_presents = 0;
    if (milliseconds(taken) > 250 && slow_presents < 20) {
        ++slow_presents;
        log_line("renderer: a Present took %.0f ms", milliseconds(taken));
    }
}

void present_thread() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(g_present_mutex);
            g_present_wake.wait(lock, [] { return g_present_pending; });
            g_present_pending = false;
        }
        present_now();
    }
}

}  // namespace

bool renderer_start(HWND window) {
    // render_scale 0 (Automatic): as many lines as the window's monitor has, for a picture that is not resampled
    // up or down in height. The game draws 480; 2160 (4K) at most.
    const float requested = setting_at_start("render_scale");
    float scale = requested;
    if (requested <= 0) {
        MONITORINFO monitor{sizeof(MONITORINFO)};
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitor);
        scale = static_cast<float>(monitor.rcMonitor.bottom - monitor.rcMonitor.top) / 480.0f;
    }
    g_scale = std::clamp(scale, 1.0f, 6.0f);
    log_line("renderer: internal resolution %.0f lines (%.3gx)%s", 480.0f * g_scale, g_scale,
             requested <= 0 ? ", automatic" : "");
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1,
                                 D3D11_SDK_VERSION, &g_device, nullptr, &g_context))) {
        log_line("renderer: no Direct3D 11 device");
        return false;
    }
    g_context.As(&g_context1);
    // The present thread uses the immediate context too (one copy per frame): D3D11 then serializes the calls.
    ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(g_context.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
        g_present_threaded = true;
    } else {
        log_line("renderer: no multithread protection, presenting on the GPU thread");
    }

    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    g_device.As(&dxgi_device);
    dxgi_device->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = g_swap_width = 640;
    desc.Height = g_swap_height = 480;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(factory->CreateSwapChainForHwnd(g_device.Get(), window, &desc, nullptr, nullptr, &g_swap_chain))) {
        log_line("renderer: cannot create the swap chain");
        return false;
    }
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);  // full screen is the window's own (nv2a.cpp)
    g_window_handle = window;
    if (g_present_threaded) std::thread(present_thread).detach();

    ComPtr<ID3DBlob> vs = compile(kVertexShader, sizeof(kVertexShader) - 1, "vs_5_0", "vertex");
    if (!vs) return false;
    g_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vertex_shader);
    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 64, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 80, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 96, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"FOG", 0, DXGI_FORMAT_R32_FLOAT, 0, 112, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    static_assert(sizeof(RenderVertex) == 116);
    g_device->CreateInputLayout(layout, 8, vs->GetBufferPointer(), vs->GetBufferSize(), &g_input_layout);

    const std::string fixed_function = std::string(kVertexDataShader) + kFixedFunctionVertexShader;
    if (ComPtr<ID3DBlob> ff = compile(fixed_function.data(), fixed_function.size(), "vs_5_0", "fixed-function vertex")) {
        g_device->CreateVertexShader(ff->GetBufferPointer(), ff->GetBufferSize(), nullptr, &g_fixed_function_vertex_shader);
    }
    D3D11_BUFFER_DESC transform_constants{};
    transform_constants.ByteWidth = sizeof(FixedFunctionConstants);
    transform_constants.Usage = D3D11_USAGE_DEFAULT;
    transform_constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_device->CreateBuffer(&transform_constants, nullptr, &g_fixed_function_constants);
    transform_constants.ByteWidth = 16;
    g_device->CreateBuffer(&transform_constants, nullptr, &g_fixed_function_draw_constants);
    transform_constants.ByteWidth = sizeof(ProgramConstants);
    g_device->CreateBuffer(&transform_constants, nullptr, &g_program_constants);
    // Writing to a buffer the vertex shader reads, without waiting (NO_OVERWRITE), needs this feature.
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    g_device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options));
    if (!options.MapNoOverwriteOnDynamicBufferSRV) {
        log_line("renderer: no NO_OVERWRITE on shader-read buffers: fixed-function transform stays on the CPU");
        g_fixed_function_vertex_shader.Reset();
    }

    D3D11_BUFFER_DESC constants{};
    constants.ByteWidth = sizeof(CombinerConstants);
    constants.Usage = D3D11_USAGE_DEFAULT;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_device->CreateBuffer(&constants, nullptr, &g_constants);

    // Clears of only some colour channels: a full-surface triangle with the clear colour, written through a mask.
    constexpr char kClearVertexShader[] =
        "float4 main(uint id : SV_VertexID) : SV_Position {\n"
        "    float2 p = float2((id << 1) & 2, id & 2);\n"
        "    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
        "}\n";
    constexpr char kClearPixelShader[] =
        "cbuffer Clear : register(b1) { float4 colour; };\n"
        "float4 main() : SV_Target { return colour; }\n";
    ComPtr<ID3DBlob> clear_vs = compile(kClearVertexShader, sizeof(kClearVertexShader) - 1, "vs_5_0", "clear vertex");
    ComPtr<ID3DBlob> clear_ps = compile(kClearPixelShader, sizeof(kClearPixelShader) - 1, "ps_5_0", "clear pixel");
    if (clear_vs && clear_ps) {
        g_device->CreateVertexShader(clear_vs->GetBufferPointer(), clear_vs->GetBufferSize(), nullptr, &g_clear_vertex_shader);
        g_device->CreatePixelShader(clear_ps->GetBufferPointer(), clear_ps->GetBufferSize(), nullptr, &g_clear_pixel_shader);
    }
    constexpr char kCopyPixelShader[] =
        "Texture2D source : register(t0);\n"
        "float4 main(float4 position : SV_Position) : SV_Target { return source.Load(int3(position.xy, 0)); }\n";
    ComPtr<ID3DBlob> copy_ps = compile(kCopyPixelShader, sizeof(kCopyPixelShader) - 1, "ps_5_0", "copy pixel");
    if (copy_ps && g_clear_vertex_shader) {
        g_copy_vertex_shader = g_clear_vertex_shader;
        g_device->CreatePixelShader(copy_ps->GetBufferPointer(), copy_ps->GetBufferSize(), nullptr, &g_copy_pixel_shader);
    }
    constexpr char kPresentVertexShader[] =
        "struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };\n"
        "Output main(uint id : SV_VertexID) {\n"
        "    Output o;\n"
        "    const float2 p = float2((id << 1) & 2, id & 2);\n"
        "    o.position = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
        "    o.uv = p;\n"
        "    return o;\n"
        "}\n";
    constexpr char kPresentPixelShader[] =
        "Texture2D frame : register(t0);\n"
        "SamplerState filtered : register(s0);\n"
        "float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
        "    return float4(frame.Sample(filtered, uv).rgb, 1);\n"
        "}\n";
    ComPtr<ID3DBlob> present_vs = compile(kPresentVertexShader, sizeof(kPresentVertexShader) - 1, "vs_5_0", "present vertex");
    ComPtr<ID3DBlob> present_ps = compile(kPresentPixelShader, sizeof(kPresentPixelShader) - 1, "ps_5_0", "present pixel");
    if (present_vs && present_ps) {
        g_device->CreateVertexShader(present_vs->GetBufferPointer(), present_vs->GetBufferSize(), nullptr,
                                     &g_present_vertex_shader);
        g_device->CreatePixelShader(present_ps->GetBufferPointer(), present_ps->GetBufferSize(), nullptr,
                                    &g_present_pixel_shader);
    }
    D3D11_SAMPLER_DESC present_sampler{};
    present_sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    present_sampler.AddressU = present_sampler.AddressV = present_sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    present_sampler.MaxLOD = D3D11_FLOAT32_MAX;
    g_device->CreateSamplerState(&present_sampler, &g_present_sampler);
    present_sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    g_device->CreateSamplerState(&present_sampler, &g_point_sampler);
    constexpr char kStretchPixelShader[] =
        "Texture2D source : register(t0);\n"
        "SamplerState nearest : register(s0);\n"
        "float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
        "    return source.Sample(nearest, uv);\n"
        "}\n";
    const auto pixel = [](const char* source, size_t size, const char* name, ComPtr<ID3D11PixelShader>& shader) {
        if (ComPtr<ID3DBlob> code = compile(source, size, "ps_5_0", name)) {
            g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader);
        }
    };
    pixel(kBrightPixelShader, sizeof(kBrightPixelShader) - 1, "bloom bright pixel", g_bright_pixel_shader);
    pixel(kBlurPixelShader, sizeof(kBlurPixelShader) - 1, "bloom blur pixel", g_blur_pixel_shader);
    pixel(kFinalPixelShader, sizeof(kFinalPixelShader) - 1, "final pixel", g_final_pixel_shader);
    pixel(kOcclusionPixelShader, sizeof(kOcclusionPixelShader) - 1, "occlusion pixel", g_occlusion_pixel_shader);
    D3D11_BUFFER_DESC occlusion_constants{};
    occlusion_constants.ByteWidth = sizeof(OcclusionConstants);
    occlusion_constants.Usage = D3D11_USAGE_DEFAULT;
    occlusion_constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_device->CreateBuffer(&occlusion_constants, nullptr, &g_occlusion_constants);
    D3D11_BLEND_DESC multiply{};
    multiply.RenderTarget[0].BlendEnable = TRUE;
    multiply.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
    multiply.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
    multiply.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    multiply.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    multiply.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    multiply.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    multiply.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    g_device->CreateBlendState(&multiply, &g_multiply_blend);
    D3D11_BUFFER_DESC post_constants{};
    post_constants.ByteWidth = sizeof(PostConstants);
    post_constants.Usage = D3D11_USAGE_DEFAULT;
    post_constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_device->CreateBuffer(&post_constants, nullptr, &g_post_constants);
    g_effects_enabled = setting("effects") != 0;
    ComPtr<ID3DBlob> stretch_ps = compile(kStretchPixelShader, sizeof(kStretchPixelShader) - 1, "ps_5_0", "stretch pixel");
    if (stretch_ps) {
        g_device->CreatePixelShader(stretch_ps->GetBufferPointer(), stretch_ps->GetBufferSize(), nullptr,
                                    &g_stretch_pixel_shader);
    }

    D3D11_BUFFER_DESC surface_constants{};
    surface_constants.ByteWidth = 16;
    surface_constants.Usage = D3D11_USAGE_DEFAULT;
    surface_constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_device->CreateBuffer(&surface_constants, nullptr, &g_surface_constants);
    D3D11_BUFFER_DESC clear_constants{};
    clear_constants.ByteWidth = 16;
    clear_constants.Usage = D3D11_USAGE_DEFAULT;
    clear_constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_device->CreateBuffer(&clear_constants, nullptr, &g_clear_constants);

    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;  // winding is not translated yet: draw both faces
    raster.DepthClipEnable = TRUE;
    raster.ScissorEnable = TRUE;  // the surface clip
    g_device->CreateRasterizerState(&raster, &g_rasterizer);
    char dump[16] = {};
    if (GetEnvironmentVariableA("MC3_DUMP_FRAMES", dump, sizeof(dump))) g_dump_every = std::strtoul(dump, nullptr, 10);
    if (GetEnvironmentVariableA("MC3_DUMP_TARGET", dump, sizeof(dump))) g_dump_target = std::strtoul(dump, nullptr, 16);
    log_line("renderer: Direct3D 11 ready");
    return true;
}

bool renderer_over_shown_scene(const SurfaceState& surface) {
    const ForgetBound forget;
    const auto scene = g_scenes.find(surface.zeta_address);
    return scene != g_scenes.end() && scene->second.drawn && g_shown_surfaces.count(surface.color_address) != 0;
}

void renderer_clear(const SurfaceState& surface, uint32_t flags, uint32_t color, uint32_t depth_stencil, int x0, int y0,
                    int x1, int y1) {
    if (!g_device || !surface.width || !surface.height) return;
    Target* target = get_target(surface.color_address, surface.width, surface.height);
    if (!target) return;
    // One more column and row than the rectangle's inclusive maximum: the game's rectangles end a pixel short of the
    // quads it draws over them. The HUD radar clears alpha on 38..165 x 323..450 and draws its background and map,
    // masked by destination alpha, on 39..166 x 324..451; the uncleared edge kept the scene's alpha (sun glare adds
    // up there) and showed a dark line to the right of and under the radar.
    x1 += 1, y1 += 1;
    // NV097_CLEAR_SURFACE: R 0x10, G 0x20, B 0x40, A 0x80 map onto D3D11's write-mask bits.
    const uint32_t channels = (flags >> 4) & 0xF;
    // That clear stays where it is while the radar's quads are narrowed towards the left (hud_narrowing): the strip
    // they gained kept the scene's alpha, and the map showed there, outside the ring. So it reaches that far too.
    static const bool hud_aspect = setting_at_start("hud_aspect") != 0;
    if (channels == 8 && hud_aspect && g_picture_width && g_picture_height && x1 - x0 <= 200 && y1 - y0 <= 200 &&
        x1 <= static_cast<int>(g_picture_width / 2) && y0 >= static_cast<int>(g_picture_height / 2) &&
        renderer_over_shown_scene(surface)) {
        const float narrow = static_cast<float>(g_picture_width) / static_cast<float>(g_picture_height) / renderer_aspect();
        if (narrow < 0.99f) x0 = static_cast<int>(static_cast<float>(x0) * narrow);
    }
    if (channels == 0xF) {
        float rgba[4];
        unpack_color(color, rgba);
        const D3D11_RECT rect{scaled(x0), scaled(y0), scaled(x1 + 1), scaled(y1 + 1)};
        g_context1->ClearView(target->rtv.Get(), rgba, &rect, 1);
    } else if (channels && g_clear_pixel_shader) {
        // Some channels only (the HUD clears alpha alone to reuse it as a mask).
        float rgba[4];
        unpack_color(color, rgba);
        g_context->UpdateSubresource(g_clear_constants.Get(), 0, nullptr, rgba, 0, 0);
        ComPtr<ID3D11BlendState>& blend = g_clear_blend_states[channels];
        if (!blend) {
            D3D11_BLEND_DESC desc{};
            desc.RenderTarget[0].SrcBlend = desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            desc.RenderTarget[0].DestBlend = desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
            desc.RenderTarget[0].BlendOp = desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            desc.RenderTarget[0].RenderTargetWriteMask = static_cast<UINT8>(channels);
            g_device->CreateBlendState(&desc, &blend);
        }
        ID3D11RenderTargetView* rtv = target->rtv.Get();
        g_context->OMSetRenderTargets(1, &rtv, nullptr);
        g_context->OMSetBlendState(blend.Get(), nullptr, 0xFFFFFFFF);
        g_context->OMSetDepthStencilState(nullptr, 0);
        const D3D11_VIEWPORT viewport{0, 0, scaled_float(target->width), scaled_float(target->height), 0, 1};
        g_context->RSSetViewports(1, &viewport);
        const D3D11_RECT rect{scaled(x0), scaled(y0), scaled(x1 + 1), scaled(y1 + 1)};
        g_context->RSSetScissorRects(1, &rect);
        g_context->RSSetState(g_rasterizer.Get());
        g_context->IASetInputLayout(nullptr);
        g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_context->VSSetShader(g_clear_vertex_shader.Get(), nullptr, 0);
        g_context->PSSetShader(g_clear_pixel_shader.Get(), nullptr, 0);
        ID3D11Buffer* constants = g_clear_constants.Get();
        g_context->PSSetConstantBuffers(1, 1, &constants);
        g_context->Draw(3, 0);
    }
    // A scene (or a split-screen view) starts. The game also clears the same depth buffer for small targets
    // (256x256, 512x512 reflections) in the middle of a frame; those must not restart the scene, or the occlusion
    // was skipped in some frames and flickered.
    if ((flags & 0x1) && surface.zeta_address && surface.width >= 640) {
        g_scenes[surface.zeta_address] = SceneState{};
        ++g_full_depth_clears;
    }
    if ((flags & 0x3) && surface.zeta_address) {
        // The depth buffer has its colour target's size (D3D11 requires it when both are bound).
        if (DepthTarget* depth = get_depth_target(surface.zeta_address, target->width, target->height)) {
            const float z = static_cast<float>(depth_stencil >> 8) / 16777215.0f;
            g_context->ClearDepthStencilView(depth->dsv.Get(),
                                             ((flags & 1) ? D3D11_CLEAR_DEPTH : 0) | ((flags & 2) ? D3D11_CLEAR_STENCIL : 0),
                                             z, static_cast<UINT8>(depth_stencil));
        }
    }
}

namespace {

// What every draw needs before its vertices: the target (grown or switched to float as needed), the depth buffer,
// the pixel shader, and the ambient occlusion pass when the 3D scene ends.
struct PreparedDraw {
    Target* target = nullptr;
    DepthTarget* depth = nullptr;
    ID3D11PixelShader* shader = nullptr;
};

bool prepare_draw(const SurfaceState& surface, const DrawState& state, PreparedDraw& draw) {
    if (!g_device || !surface.width || !surface.height) return false;
    // Culling both faces draws nothing.
    if (state.cull_enable && state.cull_face == 0x408) return false;
    ++g_stats.draws;
    Target* target = get_target(surface.color_address, surface.width, surface.height);
    if (!target) return false;
    if (state.blend_enable && (state.blend_equation == 0xF005 || state.blend_equation == 0xF006)) {
        make_float_target(*target, surface.color_address);
    }
    ID3D11PixelShader* shader = pixel_shader(state.program);
    if (!shader) return false;
    DepthTarget* depth = (surface.zeta_address && (state.depth_test || state.depth_write || state.stencil_test))
                             ? get_depth_target(surface.zeta_address, target->width, target->height)
                             : nullptr;
    // Ambient occlusion between the 3D scene and what is drawn over it (the HUD keeps depth testing on, with
    // ALWAYS and no writes, so "3D" means comparing or writing depth).
    constexpr uint32_t kDepthAlways = 0x207;
    const bool scene_draw = (state.depth_test && state.depth_function != kDepthAlways) || state.depth_write;
    if (surface.zeta_address && target->width >= 640) {
        SceneState& scene = g_scenes[surface.zeta_address];
        if (scene_draw) {
            scene.drawn = true;
        } else if (scene.drawn && !scene.occluded) {
            scene.occluded = true;
            if (DepthTarget* scene_depth = get_depth_target(surface.zeta_address, target->width, target->height)) {
                apply_occlusion(*target, *scene_depth);
                ++g_occlusion_scenes;
            }
        }
    }
    draw = PreparedDraw{target, depth, shader};
    return true;
}

// A copy of a render target's colour, for a draw that samples the target it draws into.
ID3D11ShaderResourceView* target_copy(const Target& target) {
    static ComPtr<ID3D11Texture2D> copy;
    static ComPtr<ID3D11ShaderResourceView> copy_view;
    D3D11_TEXTURE2D_DESC desc{}, existing{};
    target.texture->GetDesc(&desc);
    if (copy) copy->GetDesc(&existing);
    if (!copy || existing.Width != desc.Width || existing.Height != desc.Height || existing.Format != desc.Format ||
        existing.MipLevels != desc.MipLevels) {
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = 0;
        copy.Reset();
        copy_view.Reset();
        if (const HRESULT hr = g_device->CreateTexture2D(&desc, nullptr, &copy); FAILED(hr)) {
            log_resource_failure("a copy of the render target", hr, desc);
            return nullptr;
        }
        g_device->CreateShaderResourceView(copy.Get(), nullptr, &copy_view);
    }
    g_context->CopyResource(copy.Get(), target.texture.Get());
    return copy_view.Get();
}

// Render target, viewport, scissor, raster, blend, depth state, combiner constants, textures and samplers.
void bind_draw_state(const SurfaceState& surface, const DrawState& state, const PreparedDraw& draw) {
    Target* target = draw.target;
    const uint32_t texture = state.textures[0].enabled ? state.textures[0].descriptor.address : 0;
    const Narrowing hud =
        hud_narrowing(surface, state.screen_left, state.screen_right, state.screen_top, state.screen_bottom, texture);
    if (g_surface_scale_width != target->width || g_surface_scale_height != target->height ||
        g_surface_scale_narrow != hud.factor || g_surface_scale_anchor != hud.anchor) {
        const float scale[4] = {2.0f / target->width, 2.0f / target->height, hud.factor, hud.anchor};
        g_context->UpdateSubresource(g_surface_constants.Get(), 0, nullptr, scale, 0, 0);
        g_surface_scale_width = target->width;
        g_surface_scale_height = target->height;
        g_surface_scale_narrow = hud.factor;
        g_surface_scale_anchor = hud.anchor;
    }
    BoundState& bound = g_bound;
    if (!bound.valid) {
        bound = BoundState{};
        ID3D11Buffer* surface_constants = g_surface_constants.Get();
        g_context->VSSetConstantBuffers(0, 1, &surface_constants);
        ID3D11Buffer* constants = g_constants.Get();
        g_context->PSSetConstantBuffers(0, 1, &constants);
    }

    ID3D11RenderTargetView* rtv = target->rtv.Get();
    ID3D11DepthStencilView* dsv = draw.depth ? draw.depth->dsv.Get() : nullptr;
    if (!bound.valid || rtv != bound.rtv || dsv != bound.dsv) {
        // A surface can be both the target and a texture of the same draw; unbind it from the pixel shader first.
        ID3D11ShaderResourceView* none[4] = {};
        g_context->PSSetShaderResources(0, 4, none);
        std::fill(std::begin(bound.views), std::end(bound.views), nullptr);
        g_context->OMSetRenderTargets(1, &rtv, dsv);
        bound.rtv = rtv, bound.dsv = dsv;
    }
    const D3D11_VIEWPORT viewport{0, 0, scaled_float(target->width), scaled_float(target->height), 0, 1};
    if (!bound.valid || std::memcmp(&viewport, &bound.viewport, sizeof(viewport)) != 0) {
        g_context->RSSetViewports(1, &viewport);
        bound.viewport = viewport;
    }
    const auto clip_x0 = static_cast<uint32_t>(std::lround(hud.apply(static_cast<float>(surface.clip_x))));
    const auto clip_x1 =
        static_cast<uint32_t>(std::lround(hud.apply(static_cast<float>(surface.clip_x + surface.clip_width))));
    const D3D11_RECT scissor{scaled(clip_x0), scaled(surface.clip_y), scaled(clip_x1),
                             scaled(surface.clip_y + surface.clip_height)};
    if (!bound.valid || std::memcmp(&scissor, &bound.scissor, sizeof(scissor)) != 0) {
        g_context->RSSetScissorRects(1, &scissor);
        bound.scissor = scissor;
    }
    if (ID3D11RasterizerState* rasterizer = rasterizer_for(state); !bound.valid || rasterizer != bound.rasterizer) {
        g_context->RSSetState(rasterizer);
        bound.rasterizer = rasterizer;
    }
    if (ID3D11BlendState* blend = blend_state(state);
        !bound.valid || blend != bound.blend || std::memcmp(state.blend_color, bound.blend_color, sizeof(bound.blend_color)) != 0) {
        g_context->OMSetBlendState(blend, state.blend_color, 0xFFFFFFFF);
        bound.blend = blend;
        std::memcpy(bound.blend_color, state.blend_color, sizeof(bound.blend_color));
    }
    if (ID3D11DepthStencilState* depth = depth_state(state);
        !bound.valid || depth != bound.depth || state.stencil_reference != bound.stencil_reference) {
        g_context->OMSetDepthStencilState(depth, state.stencil_reference);
        bound.depth = depth;
        bound.stencil_reference = state.stencil_reference;
    }

    // A linear texture is addressed in texels; when it is one of our render targets, its real width (from the
    // surface pitch) can be larger than the texture's rectangle, so normalise by the render target's size.
    CombinerConstants constants_data = state.constants;
    for (int i = 0; i < 4; ++i) {
        if (!state.textures[i].enabled || !texture_is_linear(state.textures[i].descriptor)) continue;
        const auto rendered = g_targets.find(state.textures[i].descriptor.address);
        if (rendered == g_targets.end()) continue;
        constants_data.texture_scale[i][0] = 1.0f / rendered->second.width;
        constants_data.texture_scale[i][1] = 1.0f / rendered->second.height;
    }
    // The motion blur: the blur buffer added to the frame, weighted by the frame's alpha (DST_ALPHA, ONE). Setting
    // motion_blur scales it (1 = like the Xbox, 0 = off).
    if (g_blur_surface && surface.color_address != g_blur_surface && state.blend_enable && state.blend_source == 0x304 &&
        state.blend_destination == 1 && state.textures[0].enabled && state.textures[0].descriptor.address == g_blur_surface) {
        constants_data.alpha_reference[1] = 1 - std::clamp(setting("motion_blur"), 0.0f, 1.0f);
    }
    // The speed blur at high speed is made another way: the shown frame is shrunk into a surface, blended there with
    // slightly enlarged copies of itself (the streaks), and laid back over the frame by its alpha (SRC_ALPHA,
    // INV_SRC_ALPHA): setting motion_blur scales it. The lights' streaks go through the same surface: their halos
    // are drawn into it, stretched the same way and added to the frame (SRC_ALPHA, ONE): setting light_streaks.
    const bool reads_frame = state.textures[0].enabled && g_shown_surfaces.count(state.textures[0].descriptor.address);
    const bool draws_frame = g_shown_surfaces.count(surface.color_address) != 0;
    if (reads_frame && !draws_frame) g_frame_copies.insert(surface.color_address);
    if (draws_frame && state.blend_enable && state.blend_source == 0x302 &&
        (state.blend_destination == 0x303 || state.blend_destination == 1) && state.textures[0].enabled &&
        g_frame_copies.count(state.textures[0].descriptor.address)) {
        const char* key = state.blend_destination == 1 ? "light_streaks" : "motion_blur";
        constants_data.alpha_reference[2] = 1 - std::clamp(setting(key), 0.0f, 1.0f);
    }
    if (!bound.valid || std::memcmp(&constants_data, &bound.constants, sizeof(constants_data)) != 0) {
        g_context->UpdateSubresource(g_constants.Get(), 0, nullptr, &constants_data, 0, 0);
        bound.constants = constants_data;
    }

    ID3D11ShaderResourceView* views[4] = {};
    ID3D11SamplerState* samplers[4] = {};
    refresh_write_tracking();
    for (int i = 0; i < 4; ++i) {
        if (!state.textures[i].enabled) continue;
        views[i] = texture_view(state.textures[i], state.screen_left <= state.screen_right);
        samplers[i] = sampler(state.textures[i]);
        // The draw reads its own target: the NV2A then sees what is there now (MC3's motion blur writes its mask
        // into the frame's alpha this way). Direct3D cannot read and write one texture, so it reads a copy.
        if (views[i] == target->srv.Get()) {
            views[i] = target_copy(*target);
            g_blur_surface = surface.color_address;
        }
    }
    if (!bound.valid || std::memcmp(views, bound.views, sizeof(views)) != 0) {
        g_context->PSSetShaderResources(0, 4, views);
        std::memcpy(bound.views, views, sizeof(views));
    }
    if (!bound.valid || std::memcmp(samplers, bound.samplers, sizeof(samplers)) != 0) {
        g_context->PSSetSamplers(0, 4, samplers);
        std::memcpy(bound.samplers, samplers, sizeof(samplers));
    }
    if (!bound.valid || draw.shader != bound.shader) {
        g_context->PSSetShader(draw.shader, nullptr, 0);
        bound.shader = draw.shader;
    }
    bound.valid = true;
}

// Appends `bytes` to a dynamic ring buffer: written without waiting (NO_OVERWRITE), renamed (DISCARD) only when
// full. Returns the byte offset, or ~0u on failure. The buffer grows (and is recreated) when too small.
struct RingBuffer {
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11ShaderResourceView> view;  // raw (byte address) view, for buffers the vertex shader reads
    uint32_t capacity = 0, used = 0;
};

uint32_t ring_append(RingBuffer& ring, const void* data, uint32_t bytes, UINT bind, uint32_t element) {
    constexpr uint32_t kMinimumRing = 8u << 20;
    if (bytes > ring.capacity) {
        ring.capacity = std::max({bytes, ring.capacity * 2, kMinimumRing});
        ring.capacity -= ring.capacity % element;
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = ring.capacity;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = bind;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (bind & D3D11_BIND_SHADER_RESOURCE) desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        ring.buffer.Reset();
        ring.view.Reset();
        if (const HRESULT hr = g_device->CreateBuffer(&desc, nullptr, &ring.buffer); FAILED(hr)) {
            log_resource_failure("a vertex ring buffer", hr, desc);
            return ~0u;
        }
        if (bind & D3D11_BIND_SHADER_RESOURCE) {
            D3D11_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R32_TYPELESS;
            view.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
            view.BufferEx.NumElements = ring.capacity / 4;
            view.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
            if (FAILED(g_device->CreateShaderResourceView(ring.buffer.Get(), &view, &ring.view))) return ~0u;
        }
        ring.used = ring.capacity;  // forces a DISCARD below
    }
    ring.used = (ring.used + element - 1) / element * element;
    const bool wrap = ring.used + bytes > ring.capacity;
    if (wrap) ring.used = 0;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(g_context->Map(ring.buffer.Get(), 0, wrap ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0,
                              &mapped))) {
        return ~0u;
    }
    std::memcpy(static_cast<uint8_t*>(mapped.pData) + ring.used, data, bytes);
    g_context->Unmap(ring.buffer.Get(), 0);
    const uint32_t offset = ring.used;
    ring.used += bytes;
    return offset;
}

RingBuffer g_attribute_ring, g_index_ring;
FixedFunctionConstants g_fixed_function_uploaded{};
bool g_fixed_function_valid = false;

// Vertex programs translated to HLSL, by VertexProgram key. Compiled on helper threads, and kept on disk so that
// later runs find them at once; until ready (or if they do not compile) the CPU runs the program.
struct ProgramShader {
    ComPtr<ID3D11VertexShader> shader;
    enum { kPending, kReady, kFailed } state = kPending;
};
std::mutex g_program_mutex;
std::unordered_map<uint64_t, ProgramShader> g_program_shaders;  // under g_program_mutex

ProgramConstants g_program_uploaded{};
bool g_program_valid = false;

// A draw whose vertex shader reads the guest's vertex bytes (kVertexDataShader): uploads them and the indices,
// and the shader's constants (b1) when they differ from the last ones uploaded into `buffer`.
template <typename Constants>
void draw_vertex_data(const SurfaceState& surface, const DrawState& state, ID3D11VertexShader* shader,
                      ID3D11Buffer* buffer, const Constants& constants, Constants& uploaded, bool& valid,
                      const void* vertex_data, uint32_t vertex_bytes, const std::vector<uint32_t>& indices) {
    if (indices.empty() || !shader) return;
    const uint64_t draw_start = ticks();
    PreparedDraw draw;
    if (!prepare_draw(surface, state, draw)) return;
    uint64_t mark = ticks();
    g_stats.part_ticks[0] += mark - draw_start;
    uint32_t first = 0;
    if (vertex_bytes) {
        first = ring_append(g_attribute_ring, vertex_data, vertex_bytes, D3D11_BIND_SHADER_RESOURCE, 4);
        if (first == ~0u) return;
    }
    const uint32_t index_offset = ring_append(g_index_ring, indices.data(), static_cast<uint32_t>(indices.size() * 4),
                                              D3D11_BIND_INDEX_BUFFER, 4);
    if (index_offset == ~0u) return;
    // The transform state changes far less often than draws: uploaded only when different.
    if (!valid || std::memcmp(&constants, &uploaded, sizeof(constants)) != 0) {
        g_context->UpdateSubresource(buffer, 0, nullptr, &constants, 0, 0);
        uploaded = constants;
        valid = true;
    }
    const uint32_t draw_constants[4] = {first, 0, 0, 0};
    g_context->UpdateSubresource(g_fixed_function_draw_constants.Get(), 0, nullptr, draw_constants, 0, 0);
    const uint64_t uploaded_at = ticks();
    g_stats.part_ticks[1] += uploaded_at - mark;
    bind_draw_state(surface, state, draw);
    mark = ticks();
    g_stats.part_ticks[3] += mark - uploaded_at;
    ID3D11Buffer* buffers[2] = {buffer, g_fixed_function_draw_constants.Get()};
    g_context->VSSetConstantBuffers(1, 2, buffers);
    ID3D11ShaderResourceView* view = g_attribute_ring.view.Get();
    g_context->VSSetShaderResources(0, 1, &view);
    g_context->IASetInputLayout(nullptr);
    g_context->IASetIndexBuffer(g_index_ring.buffer.Get(), DXGI_FORMAT_R32_UINT, 0);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(shader, nullptr, 0);
    g_context->DrawIndexed(static_cast<UINT>(indices.size()), index_offset / 4, 0);
    const uint64_t end = ticks();
    g_stats.part_ticks[4] += end - mark;
    g_stats.draw_ticks += end - draw_start;
}

}  // namespace

bool renderer_load_picture(const std::filesystem::path& file, std::vector<uint8_t>& bgra, UINT& width, UINT& height) {
    return load_picture(file, bgra, width, height);
}

bool renderer_has_fixed_function() { return g_fixed_function_vertex_shader != nullptr; }

void renderer_draw_fixed_function(const SurfaceState& surface, const DrawState& state,
                                  const FixedFunctionConstants& constants, const void* vertex_data,
                                  uint32_t vertex_bytes, const std::vector<uint32_t>& indices) {
    draw_vertex_data(surface, state, g_fixed_function_vertex_shader.Get(), g_fixed_function_constants.Get(), constants,
                     g_fixed_function_uploaded, g_fixed_function_valid, vertex_data, vertex_bytes, indices);
}

bool renderer_prepare_program(uint64_t key, const std::function<std::string()>& source) {
    if (!g_fixed_function_vertex_shader || !g_program_constants) return false;  // no GPU vertex path at all
    {
        std::lock_guard<std::mutex> lock(g_program_mutex);
        const auto found = g_program_shaders.find(key);
        if (found != g_program_shaders.end()) return found->second.state == ProgramShader::kReady;
        g_program_shaders[key].state = ProgramShader::kPending;
    }
    const std::string text = std::string(kVertexDataShader) + kVertexProgramShader + source();
    const std::filesystem::path file = shader_cache_file("vp", text);
    const auto finish = [key](ComPtr<ID3D11VertexShader> shader) {
        std::lock_guard<std::mutex> lock(g_program_mutex);
        ProgramShader& entry = g_program_shaders[key];
        entry.shader = shader;
        entry.state = shader ? ProgramShader::kReady : ProgramShader::kFailed;
    };

    std::ifstream cached(file, std::ios::binary);
    const std::vector<char> code((std::istreambuf_iterator<char>(cached)), std::istreambuf_iterator<char>());
    ComPtr<ID3D11VertexShader> shader;
    if (!code.empty() && SUCCEEDED(g_device->CreateVertexShader(code.data(), code.size(), nullptr, &shader))) {
        finish(shader);
        return true;
    }
    // Compiling takes a few hundred milliseconds: on a helper thread, while the CPU draws with this program.
    std::thread([key, text, file, finish] {
        char name[48];
        std::snprintf(name, sizeof(name), "vertex program %016llX", static_cast<unsigned long long>(key));
        const uint64_t start = ticks();
        ComPtr<ID3D11VertexShader> shader;
        if (ComPtr<ID3DBlob> code = compile(text.data(), text.size(), "vs_5_0", name)) {
            if (SUCCEEDED(g_device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                                       &shader))) {
                std::error_code error;
                std::filesystem::create_directories(file.parent_path(), error);
                std::ofstream(file, std::ios::binary)
                    .write(static_cast<const char*>(code->GetBufferPointer()), code->GetBufferSize());
            }
        }
        log_line("renderer: %s %s in %.1f ms", name, shader ? "compiled" : "stays on the CPU",
                 milliseconds(ticks() - start));
        if (!shader) log_line("%s", text.c_str() + std::strlen(kVertexDataShader) + std::strlen(kVertexProgramShader));
        finish(shader);
    }).detach();
    return false;
}

void renderer_draw_program(const SurfaceState& surface, const DrawState& state, uint64_t key,
                           const ProgramConstants& constants, const void* vertex_data, uint32_t vertex_bytes,
                           const std::vector<uint32_t>& indices) {
    ID3D11VertexShader* shader = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_program_mutex);
        const auto found = g_program_shaders.find(key);
        if (found != g_program_shaders.end()) shader = found->second.shader.Get();
    }
    draw_vertex_data(surface, state, shader, g_program_constants.Get(), constants, g_program_uploaded, g_program_valid,
                     vertex_data, vertex_bytes, indices);
}

void renderer_draw(const SurfaceState& surface, const DrawState& state, const std::vector<RenderVertex>& triangles) {
    if (triangles.empty()) return;
    const uint64_t draw_start = ticks();
    struct Account {
        uint64_t start;
        ~Account() { g_stats.draw_ticks += ticks() - start; }
    } account{draw_start};
    PreparedDraw draw;
    if (!prepare_draw(surface, state, draw)) return;
    uint64_t mark = ticks();
    const auto part = [&mark](int index) {
        const uint64_t now = ticks();
        g_stats.part_ticks[index] += now - mark;
        mark = now;
    };
    g_stats.part_ticks[0] += mark - draw_start;

    // Vertices go into a ring buffer: appended without waiting (NO_OVERWRITE), and the buffer is renamed (DISCARD)
    // only when it is full, instead of once per draw.
    const uint32_t bytes = static_cast<uint32_t>(triangles.size() * sizeof(RenderVertex));
    constexpr uint32_t kMinimumRing = 8u << 20;
    if (bytes > g_vertex_capacity) {
        g_vertex_capacity = std::max({bytes, g_vertex_capacity * 2, kMinimumRing});
        g_vertex_capacity -= g_vertex_capacity % sizeof(RenderVertex);
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = g_vertex_capacity;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        g_vertex_buffer.Reset();
        g_device->CreateBuffer(&desc, nullptr, &g_vertex_buffer);
        g_vertex_used = g_vertex_capacity;  // forces a DISCARD below
    }
    const bool wrap = g_vertex_used + bytes > g_vertex_capacity;
    if (wrap) g_vertex_used = 0;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(g_context->Map(g_vertex_buffer.Get(), 0, wrap ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0,
                              &mapped))) {
        return;
    }
    // One sequential copy: the buffer is write-combined memory, slow to write piecewise. The vertex shader maps
    // pixels to clip space over the render target's real size.
    const UINT first_vertex = g_vertex_used / sizeof(RenderVertex);
    std::memcpy(static_cast<uint8_t*>(mapped.pData) + g_vertex_used, triangles.data(), bytes);
    g_context->Unmap(g_vertex_buffer.Get(), 0);
    g_vertex_used += bytes;
    part(1);
    bind_draw_state(surface, state, draw);
    part(3);

    const UINT stride = sizeof(RenderVertex), offset = 0;
    ID3D11Buffer* buffer = g_vertex_buffer.Get();
    g_context->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
    g_context->IASetInputLayout(g_input_layout.Get());
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_vertex_shader.Get(), nullptr, 0);
    g_context->Draw(static_cast<UINT>(triangles.size()), first_vertex);
    part(4);
}

void renderer_dump_target(uint32_t address, const char* prefix) {
    const auto found = g_targets.find(address);
    if (found != g_targets.end()) dump_frame(found->second.texture.Get(), prefix);
}

bool renderer_probe(uint32_t address, uint32_t x, uint32_t y, uint8_t bgra[4]) {
    const auto found = g_targets.find(address);
    if (found == g_targets.end() || x >= found->second.width || y >= found->second.height) return false;
    static ComPtr<ID3D11Texture2D> staging;
    if (!staging) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = 1;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (const HRESULT hr = g_device->CreateTexture2D(&desc, nullptr, &staging); FAILED(hr)) {
            log_resource_failure("a staging copy", hr, desc);
            return false;
        }
    }
    const D3D11_BOX box{scaled_size(x), scaled_size(y), 0, scaled_size(x) + 1, scaled_size(y) + 1, 1};
    g_context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, found->second.texture.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(g_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    std::memcpy(bgra, mapped.pData, 4);
    g_context->Unmap(staging.Get(), 0);
    return true;
}

void renderer_note_vertex_work(uint64_t elapsed, uint32_t vertices) {
    g_stats.vertex_ticks += elapsed;
    g_stats.vertices += vertices;
}

void renderer_blit(const BlitRequest& b) {
    if (!g_device || !b.width || !b.height) return;
    const auto source = g_targets.find(b.source);
    if (source != g_targets.end() && b.bytes_per_pixel == 4) {
        const Target& from = source->second;
        if (b.source_x + b.width > from.width || b.source_y + b.height > from.height) return;
        const auto existing = g_targets.find(b.destination);
        const uint32_t width = existing != g_targets.end() ? existing->second.width : b.destination_pitch / 4;
        const uint32_t height = existing != g_targets.end() ? existing->second.height : from.height;
        Target* to = get_target(b.destination, width, height);
        if (!to || b.destination_x + b.width > to->width || b.destination_y + b.height > to->height) return;
        // With a scale that is not whole, the rounded source and destination may differ by a pixel: the copy stays
        // inside the destination texture.
        const uint32_t to_x = scaled_size(b.destination_x), to_y = scaled_size(b.destination_y);
        const uint32_t from_x = scaled_size(b.source_x), from_y = scaled_size(b.source_y);
        const uint32_t copy_width = std::min(scaled_size(b.source_x + b.width) - from_x, scaled_size(to->width) - to_x);
        const uint32_t copy_height =
            std::min(scaled_size(b.source_y + b.height) - from_y, scaled_size(to->height) - to_y);
        const D3D11_BOX box{from_x, from_y, 0, from_x + copy_width, from_y + copy_height, 1};
        g_context->CopySubresourceRegion(to->texture.Get(), 0, to_x, to_y, 0, from.texture.Get(), 0, &box);
        return;
    }
    // Guest memory to guest memory.
    const uint32_t row = b.width * b.bytes_per_pixel;
    for (uint32_t y = 0; y < b.height; ++y) {
        const uint32_t from = b.source + (b.source_y + y) * b.source_pitch + b.source_x * b.bytes_per_pixel;
        const uint32_t to = b.destination + (b.destination_y + y) * b.destination_pitch + b.destination_x * b.bytes_per_pixel;
        uint8_t* source_row = physical_pointer(from);
        uint8_t* destination_row = physical_pointer(to);
        if (!source_row || !destination_row || !physical_pointer(from + row - 1) || !physical_pointer(to + row - 1)) return;
        std::memmove(destination_row, source_row, row);
    }
    const auto destination = g_targets.find(b.destination);
    if (destination != g_targets.end() && b.bytes_per_pixel == 4) {
        Target& to = destination->second;
        if (b.destination_x + b.width > to.width || b.destination_y + b.height > to.height) return;
        const uint8_t* pixels =
            physical_pointer(b.destination + b.destination_y * b.destination_pitch + b.destination_x * 4);
        if (g_scale == 1) {
            const D3D11_BOX box{b.destination_x, b.destination_y, 0, b.destination_x + b.width,
                                b.destination_y + b.height, 1};
            g_context->UpdateSubresource(to.texture.Get(), 0, &box, pixels, b.destination_pitch, 0);
        } else {
            upload_scaled(to, b.destination_x, b.destination_y, b.width, b.height, pixels, b.destination_pitch);
        }
    }
}

float renderer_aspect() { return setting_at_start("widescreen") != 0 ? 16.0f / 9.0f : 4.0f / 3.0f; }

HudPicture renderer_hud_picture() { return g_hud_picture; }

void renderer_toggle_effects() { renderer_set_effects(!g_effects_enabled); }

void renderer_set_effects(bool on) {
    g_effects_enabled = on;
    log_line("renderer: modern effects %s", on ? "on" : "off");
}

void renderer_set_overlay(RendererOverlay overlay) { g_overlay = overlay; }

void renderer_present(uint32_t address, uint32_t width, uint32_t height, uint32_t pitch) {
    const ForgetBound forget;  // presenting draws effects and the overlay (ImGui) with their own state
    if (!g_device || !width || !height) return;
    ID3D11Texture2D* frame = nullptr;
    ID3D11ShaderResourceView* frame_view = nullptr;
    const auto target = g_targets.find(address);
    if (target != g_targets.end()) {
        g_shown_surfaces.insert(address);
        g_picture_width = width, g_picture_height = height;
        const float aspect = renderer_aspect();
        float window_width = static_cast<float>(g_swap_width), window_height = static_cast<float>(g_swap_height);
        if (window_width > window_height * aspect) {
            window_width = window_height * aspect;
        } else {
            window_height = window_width / aspect;
        }
        const float edge_x = static_cast<float>(width / kOverscan), edge_y = static_cast<float>(height / kOverscan);
        g_hud_picture = {edge_x,
                         edge_y,
                         static_cast<float>(width) - 2 * edge_x,
                         static_cast<float>(height) - 2 * edge_y,
                         std::floor((static_cast<float>(g_swap_width) - window_width) / 2),
                         std::floor((static_cast<float>(g_swap_height) - window_height) / 2),
                         window_width,
                         window_height};
        frame = target->second.texture.Get();
        frame_view = target->second.srv.Get();
        // Only the picture's part of a wider surface (widescreen: 720 of 768 pixels), copied out once per present.
        // Like a television, the outermost pixels are not shown either: games park things there that the TV's
        // overscan hid (MC3 slides its notification box off to the left and leaves its last 3 pixels on screen, and
        // when it is in, the box stops 13 pixels short of the edge, which a TV's overscan covered). 1/48 of each side,
        // the same share across and down so the picture keeps its shape.
        D3D11_TEXTURE2D_DESC desc{};
        frame->GetDesc(&desc);
        const uint32_t full_width = std::min(scaled_size(width), desc.Width);
        const uint32_t full_height = std::min(scaled_size(height), desc.Height);
        const uint32_t crop_x = full_width / kOverscan, crop_y = full_height / kOverscan;
        const uint32_t visible_width = full_width - 2 * crop_x, visible_height = full_height - 2 * crop_y;
        {
            static ComPtr<ID3D11Texture2D> visible;
            static ComPtr<ID3D11ShaderResourceView> visible_view;
            D3D11_TEXTURE2D_DESC visible_desc{};
            if (visible) visible->GetDesc(&visible_desc);
            if (!visible || visible_desc.Width != visible_width || visible_desc.Height != visible_height ||
                visible_desc.Format != desc.Format) {
                visible_desc = desc;
                visible_desc.Width = visible_width;
                visible_desc.Height = visible_height;
                visible_desc.MipLevels = 1;
                visible_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                visible_desc.MiscFlags = 0;
                visible.Reset();
                visible_view.Reset();
                if (SUCCEEDED(g_device->CreateTexture2D(&visible_desc, nullptr, &visible))) {
                    g_device->CreateShaderResourceView(visible.Get(), nullptr, &visible_view);
                }
            }
            if (visible_view) {
                const D3D11_BOX box{crop_x, crop_y, 0, crop_x + visible_width, crop_y + visible_height, 1};
                g_context->CopySubresourceRegion(visible.Get(), 0, 0, 0, 0, frame, 0, &box);
                frame = visible.Get();
                frame_view = visible_view.Get();
            }
        }
    } else {
        // Nothing rendered there: show the guest memory (32-bit pixels).
        const uint8_t* pixels = physical_pointer(address);
        if (!pixels || !pitch || !physical_pointer(address + pitch * height - 1)) return;
        if (!g_upload || g_upload_width != width || g_upload_height != height) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            g_upload.Reset();
            g_upload_srv.Reset();
            g_device->CreateTexture2D(&desc, nullptr, &g_upload);
            g_device->CreateShaderResourceView(g_upload.Get(), nullptr, &g_upload_srv);
            g_upload_width = width;
            g_upload_height = height;
        }
        g_context->UpdateSubresource(g_upload.Get(), 0, nullptr, pixels, pitch, 0);
        frame = g_upload.Get();
        frame_view = g_upload_srv.Get();
    }
    if (g_presented % 600 == 599) {
        log_line("renderer: ambient occlusion on %u of %u scenes", g_occlusion_scenes, g_full_depth_clears);
        g_occlusion_scenes = g_full_depth_clears = 0;
    }
    D3D11_TEXTURE2D_DESC frame_desc{};
    if (frame) frame->GetDesc(&frame_desc);
    ensure_swap_chain_size();
    const uint64_t effects_start = ticks();
    std::unique_lock<std::mutex> drawing(g_present_mutex);
    present_frame(frame_view, frame_desc.Width, frame_desc.Height);
    hud_frame_shown();
    ++g_presented;
    ++g_stats.presents;
    log_statistics();
    const uint64_t overlay_start = ticks();
    if (const RendererOverlay overlay = g_overlay.load(); overlay && g_final_rtv) {
        overlay(g_device.Get(), g_context.Get(), g_final_rtv.Get(), g_swap_width, g_swap_height);
    }
    const uint64_t overlay_end = ticks();
    // MC3_DUMP_SECONDS=<s>: a frame every s seconds of real time (frame-<milliseconds>.bmp... named by count).
    static const double dump_seconds = [] {
        char value[16] = {};
        return GetEnvironmentVariableA("MC3_DUMP_SECONDS", value, sizeof(value)) ? std::atof(value) : 0.0;
    }();
    static ULONGLONG next_timed_dump = 0;
    bool timed_dump = false;
    if (dump_seconds > 0) {
        const ULONGLONG now = GetTickCount64();
        if (!next_timed_dump) next_timed_dump = now;
        if (now >= next_timed_dump) {
            timed_dump = true;
            // From now: a save slower than the interval (a large picture) would otherwise make every frame due.
            next_timed_dump = now + static_cast<ULONGLONG>(dump_seconds * 1000);
        }
    }
    if ((g_dump_every && g_presented % g_dump_every == 0) || timed_dump) {
        dump_frame(frame);  // the game's frame at its own size, not the window
        static const bool dump_window = GetEnvironmentVariableA("MC3_DUMP_WINDOW", nullptr, 0) != 0;
        if (dump_window) dump_frame(g_final_texture.Get(), "window");  // with the effects and the overlay
        const auto extra = g_targets.find(g_dump_target);
        if (extra != g_targets.end()) dump_frame(extra->second.texture.Get(), "target");
    }
    if (g_present_threaded) {
        g_present_pending = true;
        drawing.unlock();
        g_present_wake.notify_one();
    } else {
        drawing.unlock();
        present_now();
    }
    // Drawing that holds the GPU thread up holds up the game too (it waits for the GPU to take its commands): which
    // part took the time, for the first few.
    static int slow_frames = 0;
    const double total = milliseconds(overlay_end - effects_start);
    if (total > 250 && slow_frames < 20) {
        ++slow_frames;
        log_line("renderer: drawing the window took %.0f ms: effects %.0f, overlay %.0f", total,
                 milliseconds(overlay_start - effects_start), milliseconds(overlay_end - overlay_start));
    }
}

}  // namespace mc3::pc
