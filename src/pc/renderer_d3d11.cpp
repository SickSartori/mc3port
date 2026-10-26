#include "pc/renderer_d3d11.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>

#include "pc/log.h"
#include "pc/physical_memory.h"
#include "pc/settings.h"

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
// Positions arrive in pixels of the render target times w; scale = (2 / width, 2 / height).
cbuffer Surface : register(b0) { float4 scale; };
PSInput main(VSInput i) {
    PSInput o;
    o.position = float4(i.position.x * scale.x - i.position.w, i.position.w - i.position.y * scale.y,
                        i.position.z, i.position.w);
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
    if (isinf(distance)) return mode == 0x801 || mode == 0x803 ? 0 : 1;
    float factor = 1;
    if (mode == 0x2601 || mode == 0x804) factor = params.x + distance * params.y - 1;
    else if (mode == 0x800 || mode == 0x802) factor = params.x + exp2(distance * params.y * 16) - 1.5;
    else if (mode == 0x801 || mode == 0x803) factor = params.x + exp2(-distance * distance * params.y * params.y * 32) - 1.5;
    if (mode == 0x802 || mode == 0x803 || mode == 0x804) factor = abs(factor);
    return isnan(factor) ? 1 : factor;
}

// A screen-space position (pixels; w apart) to clip space, as to_render_vertex and then the main vertex shader do.
float4 clip_position(float4 screen, float clip_max) {
    float w = screen.w;
    if (!isfinite(w) || abs(w) < 5.42101e-20) w = w < 0 ? -5.42101e-20 : 5.42101e-20;
    const float z = clip_max != 0 ? screen.z / clip_max : screen.z;
    return float4(screen.x * w * scale.x - w, w - screen.y * w * scale.y, z * w, w);
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
    if (!isfinite(w) || abs(w) < 5.42101e-20) w = w < 0 ? -5.42101e-20 : 5.42101e-20;
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
    if (isnan(r)) return 5.42101e-20;
    const float magnitude = clamp(abs(r), 5.42101e-20, 1.884467e+19);
    return (asuint(r) >> 31) ? -magnitude : magnitude;
}
float4 rsq(float s) { return s == 0 ? kInfinity : isinf(s) ? 0 : 1 / sqrt(abs(s)); }
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
};

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
ComPtr<ID3D11VertexShader> g_clear_vertex_shader;
ComPtr<ID3D11PixelShader> g_clear_pixel_shader;
ComPtr<ID3D11Buffer> g_clear_constants;
std::map<uint32_t, ComPtr<ID3D11BlendState>> g_clear_blend_states;  // by write mask
ComPtr<ID3D11Texture2D> g_upload;  // guest framebuffers that were never rendered to
ComPtr<ID3D11ShaderResourceView> g_upload_srv;
uint32_t g_upload_width = 0, g_upload_height = 0;

// Presentation: the frame is drawn into the window keeping the game's 4:3 shape, black bars around it.
HWND g_window_handle = nullptr;
ComPtr<ID3D11RenderTargetView> g_back_buffer_rtv;
ComPtr<ID3D11VertexShader> g_present_vertex_shader;
ComPtr<ID3D11PixelShader> g_present_pixel_shader;
ComPtr<ID3D11SamplerState> g_present_sampler;

std::unordered_map<uint32_t, Target> g_targets;
std::unordered_map<uint64_t, DepthTarget> g_depth_targets;
std::unordered_map<uint64_t, ComPtr<ID3D11PixelShader>> g_pixel_shaders;
std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>, CachedTexture> g_textures;
std::map<std::tuple<bool, uint32_t, uint32_t, uint32_t, uint32_t>, ComPtr<ID3D11BlendState>> g_blend_states;
std::map<std::tuple<bool, uint32_t, bool, bool, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>,
         ComPtr<ID3D11DepthStencilState>>
    g_depth_states;
std::map<std::pair<uint32_t, uint32_t>, ComPtr<ID3D11SamplerState>> g_samplers;

// Statistics, logged every 10 s from renderer_present: where the GPU thread's time goes.
struct Statistics {
    uint64_t presents = 0, draws = 0, vertices = 0, vertex_ticks = 0;
    uint64_t hashed_bytes = 0, hash_ticks = 0, uploads = 0, upload_ticks = 0, draw_ticks = 0;
    // Parts of draw_ticks: target/shader lookup, vertex upload, state, textures, the Draw call.
    uint64_t part_ticks[5] = {};
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
    log_line("renderer: D3D parts per second: setup %.0f ms, vertex upload %.0f ms, state %.0f ms, textures %.0f ms, "
             "draw %.0f ms",
             milliseconds(g_stats.part_ticks[0]) / seconds, milliseconds(g_stats.part_ticks[1]) / seconds,
             milliseconds(g_stats.part_ticks[2]) / seconds, milliseconds(g_stats.part_ticks[3]) / seconds,
             milliseconds(g_stats.part_ticks[4]) / seconds);
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

ComPtr<ID3DBlob> compile(const char* source, size_t size, const char* profile, const char* name) {
    ComPtr<ID3DBlob> code, errors;
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
// read when the renderer starts).
uint32_t g_scale = 1;

LONG scaled(uint32_t pixels) { return static_cast<LONG>(pixels * g_scale); }
float scaled_float(uint32_t pixels) { return static_cast<float>(pixels * g_scale); }

// The surface at `address`, at least width x height. A smaller request reuses the existing target (the draw's
// viewport covers only its part); a larger one grows it and keeps what was drawn so far. Sizes are the game's;
// the texture is g_scale times larger.
Target* get_target(uint32_t address, uint32_t width, uint32_t height) {
    Target& target = g_targets[address];
    if (target.texture && target.width >= width && target.height >= height) return &target;
    width = std::max(width, target.width);
    height = std::max(height, target.height);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width * g_scale;
    desc.Height = height * g_scale;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = target.format;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Target grown{};
    grown.format = target.format;
    if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &grown.texture))) return nullptr;
    g_device->CreateRenderTargetView(grown.texture.Get(), nullptr, &grown.rtv);
    g_device->CreateShaderResourceView(grown.texture.Get(), nullptr, &grown.srv);
    grown.width = width;
    grown.height = height;
    if (target.texture) {
        const D3D11_BOX box{0, 0, 0, target.width * g_scale, target.height * g_scale, 1};
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
void make_float_target(Target& target, uint32_t address) {
    if (target.format == DXGI_FORMAT_R16G16B16A16_FLOAT || !g_copy_pixel_shader) return;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = target.width * g_scale;
    desc.Height = target.height * g_scale;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Target converted{};
    converted.format = desc.Format;
    converted.width = target.width;
    converted.height = target.height;
    if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &converted.texture))) return;
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
    desc.Width = width * g_scale;
    desc.Height = height * g_scale;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R24G8_TYPELESS;  // depth-stencil view and shader view of the same texture
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    target = DepthTarget{};
    if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &target.texture))) return nullptr;
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

// Per depth buffer, since its last clear (the start of a scene; presents do not line up with scenes): 3D drawn,
// occlusion applied.
struct SceneState {
    bool drawn = false;
    bool occluded = false;
};
std::unordered_map<uint32_t, SceneState> g_scenes;
uint32_t g_occlusion_scenes = 0, g_full_depth_clears = 0;  // for the log: every scene should get its pass

void apply_occlusion(Target& target, DepthTarget& depth) {
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
    static const UINT anisotropy = std::clamp<UINT>(static_cast<UINT>(setting("anisotropy")), 1, 16);
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

ID3D11ShaderResourceView* texture_view(const TextureStage& stage) {
    const TextureDescriptor& d = stage.descriptor;
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
    CachedTexture& cached = g_textures[std::make_tuple(d.address, d.format, d.pitch, d.rect, d.palette)];
    // Unchanged memory needs no look at all; written memory is hashed, and decoded again only if it differs.
    uint32_t written = physical_write_generation(d.address, bytes);
    if (d.palette) written = std::max(written, physical_write_generation(d.palette, 1024));
    if (cached.srv && written <= cached.checked_generation) return cached.srv.Get();

    const uint64_t hash_start = ticks();
    const uint64_t hash = hash_memory(memory, bytes) ^ (d.palette ? hash_memory(physical_pointer(d.palette), 1024) : 0);
    g_stats.hash_ticks += ticks() - hash_start;
    g_stats.hashed_bytes += bytes;
    cached.checked_generation = g_write_generation;
    if (cached.srv && cached.content_hash == hash) return cached.srv.Get();

    const uint64_t upload_start = ticks();
    struct Account {
        uint64_t start;
        ~Account() {
            g_stats.upload_ticks += ticks() - start;
            ++g_stats.uploads;
        }
    } account{upload_start};
    const TextureImage image = decode_texture(d);
    if (image.layout == TextureLayout::Unsupported) return nullptr;
    // MC3_DUMP_TEXTURE=<hex address>: save the textures in that 4 KB page once decoded (BGRA with alpha) as
    // texture-<address>.bmp (resources move a little between runs).
    static const uint32_t dump_texture = [] {
        char value[16] = {};
        return GetEnvironmentVariableA("MC3_DUMP_TEXTURE", value, sizeof(value)) ? std::strtoul(value, nullptr, 16) : 0u;
    }();
    if (dump_texture && (d.address & ~0xFFFu) == (dump_texture & ~0xFFFu) && image.layout == TextureLayout::Bgra8) {
        char name[64];
        std::snprintf(name, sizeof(name), "texture-%08X.bmp", d.address);
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
    for (uint32_t level = 1; level < levels; ++level) {
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
    if (FAILED(g_device->CreateTexture2D(&desc, data.data(), &cached.texture))) return nullptr;
    g_device->CreateShaderResourceView(cached.texture.Get(), nullptr, &cached.srv);
    cached.content_hash = hash;
    return cached.srv.Get();
}

ID3D11PixelShader* pixel_shader(const CombinerProgram& program) {
    const uint64_t key = combiner_hash(program);
    const auto it = g_pixel_shaders.find(key);
    if (it != g_pixel_shaders.end()) return it->second.Get();
    ComPtr<ID3D11PixelShader>& shader = g_pixel_shaders[key];
    const std::string source = generate_pixel_shader(program);
    if (ComPtr<ID3DBlob> code = compile(source.c_str(), source.size(), "ps_5_0", "combiner")) {
        g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader);
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
    if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &staging))) return;
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
    RECT client{};
    GetClientRect(g_window_handle, &client);
    const auto width = static_cast<uint32_t>(std::max<LONG>(1, client.right - client.left));
    const auto height = static_cast<uint32_t>(std::max<LONG>(1, client.bottom - client.top));
    if (width == g_swap_width && height == g_swap_height && g_back_buffer_rtv) return;
    g_context->OMSetRenderTargets(0, nullptr, nullptr);
    g_back_buffer_rtv.Reset();
    g_swap_chain->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    g_swap_width = width;
    g_swap_height = height;
    ComPtr<ID3D11Texture2D> back_buffer;
    g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    g_device->CreateRenderTargetView(back_buffer.Get(), nullptr, &g_back_buffer_rtv);
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

// Draws the frame into the window: the largest 4:3 rectangle that fits, centred, filtered, with the effects.
void present_frame(ID3D11ShaderResourceView* frame, uint32_t frame_width, uint32_t frame_height) {
    ensure_swap_chain_size();
    if (!g_back_buffer_rtv || !g_present_pixel_shader) return;
    if (g_effects_enabled && g_final_pixel_shader && frame_width && frame_height) {
        constexpr float kAspect = 4.0f / 3.0f;
        float width = static_cast<float>(g_swap_width), height = static_cast<float>(g_swap_height);
        if (width > height * kAspect) {
            width = height * kAspect;
        } else {
            height = width / kAspect;
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
        g_context->ClearRenderTargetView(g_back_buffer_rtv.Get(), black);
        constants.output_texel[0] = 1.0f / width, constants.output_texel[1] = 1.0f / height;
        ID3D11ShaderResourceView* sources[2] = {frame, glow};
        post_pass(g_back_buffer_rtv.Get(), area, g_final_pixel_shader.Get(), sources, 2, constants);
        return;
    }
    constexpr float kAspect = 4.0f / 3.0f;
    float width = static_cast<float>(g_swap_width), height = static_cast<float>(g_swap_height);
    if (width > height * kAspect) {
        width = height * kAspect;
    } else {
        height = width / kAspect;
    }
    const float left = std::floor((g_swap_width - width) / 2), top = std::floor((g_swap_height - height) / 2);
    const float black[4] = {0, 0, 0, 1};
    g_context->ClearRenderTargetView(g_back_buffer_rtv.Get(), black);
    ID3D11RenderTargetView* rtv = g_back_buffer_rtv.Get();
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
        if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &g_stretch_source))) return;
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

}  // namespace

bool renderer_start(HWND window) {
    g_scale = std::clamp<uint32_t>(static_cast<uint32_t>(setting("render_scale")), 1, 4);
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1,
                                 D3D11_SDK_VERSION, &g_device, nullptr, &g_context))) {
        log_line("renderer: no Direct3D 11 device");
        return false;
    }
    g_context.As(&g_context1);

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
    if (channels == 0xF) {
        float rgba[4];
        unpack_color(color, rgba);
        const D3D11_RECT rect{x0 * scaled(1), y0 * scaled(1), (x1 + 1) * scaled(1), (y1 + 1) * scaled(1)};
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
        const D3D11_RECT rect{x0 * scaled(1), y0 * scaled(1), (x1 + 1) * scaled(1), (y1 + 1) * scaled(1)};
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

// Render target, viewport, scissor, raster, blend, depth state, combiner constants, textures and samplers.
void bind_draw_state(const SurfaceState& surface, const DrawState& state, const PreparedDraw& draw) {
    Target* target = draw.target;
    if (g_surface_scale_width != target->width || g_surface_scale_height != target->height) {
        const float scale[4] = {2.0f / target->width, 2.0f / target->height, 0, 0};
        g_context->UpdateSubresource(g_surface_constants.Get(), 0, nullptr, scale, 0, 0);
        g_surface_scale_width = target->width;
        g_surface_scale_height = target->height;
    }
    ID3D11Buffer* surface_constants = g_surface_constants.Get();
    g_context->VSSetConstantBuffers(0, 1, &surface_constants);

    ID3D11RenderTargetView* rtv = target->rtv.Get();
    // A surface can be both the target and a texture of the same draw; unbind it from the pixel shader first.
    ID3D11ShaderResourceView* none[4] = {};
    g_context->PSSetShaderResources(0, 4, none);
    g_context->OMSetRenderTargets(1, &rtv, draw.depth ? draw.depth->dsv.Get() : nullptr);
    const D3D11_VIEWPORT viewport{0, 0, scaled_float(target->width), scaled_float(target->height), 0, 1};
    g_context->RSSetViewports(1, &viewport);
    const D3D11_RECT scissor{scaled(surface.clip_x), scaled(surface.clip_y), scaled(surface.clip_x + surface.clip_width),
                             scaled(surface.clip_y + surface.clip_height)};
    g_context->RSSetScissorRects(1, &scissor);
    g_context->RSSetState(rasterizer_for(state));
    g_context->OMSetBlendState(blend_state(state), state.blend_color, 0xFFFFFFFF);
    g_context->OMSetDepthStencilState(depth_state(state), state.stencil_reference);

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
    g_context->UpdateSubresource(g_constants.Get(), 0, nullptr, &constants_data, 0, 0);
    ID3D11Buffer* constants = g_constants.Get();
    g_context->PSSetConstantBuffers(0, 1, &constants);

    ID3D11ShaderResourceView* views[4] = {};
    ID3D11SamplerState* samplers[4] = {};
    refresh_write_tracking();
    for (int i = 0; i < 4; ++i) {
        if (!state.textures[i].enabled) continue;
        views[i] = texture_view(state.textures[i]);
        samplers[i] = sampler(state.textures[i]);
        if (views[i] == target->srv.Get()) views[i] = nullptr;  // feedback loop
    }
    g_context->PSSetShaderResources(0, 4, views);
    g_context->PSSetSamplers(0, 4, samplers);
    g_context->PSSetShader(draw.shader, nullptr, 0);
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
        if (FAILED(g_device->CreateBuffer(&desc, nullptr, &ring.buffer))) return ~0u;
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

std::filesystem::path program_cache_folder() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path() / "shader_cache";
}
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
    g_stats.part_ticks[1] += ticks() - draw_start;
    bind_draw_state(surface, state, draw);
    ID3D11Buffer* buffers[2] = {buffer, g_fixed_function_draw_constants.Get()};
    g_context->VSSetConstantBuffers(1, 2, buffers);
    ID3D11ShaderResourceView* view = g_attribute_ring.view.Get();
    g_context->VSSetShaderResources(0, 1, &view);
    g_context->IASetInputLayout(nullptr);
    g_context->IASetIndexBuffer(g_index_ring.buffer.Get(), DXGI_FORMAT_R32_UINT, 0);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(shader, nullptr, 0);
    g_context->DrawIndexed(static_cast<UINT>(indices.size()), index_offset / 4, 0);
    g_stats.draw_ticks += ticks() - draw_start;
}

}  // namespace

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
    uint64_t hash = 14695981039346656037ull;  // the file name: any change of the shader text is a new file
    for (const char ch : text) hash = (hash ^ static_cast<uint8_t>(ch)) * 1099511628211ull;
    char file_name[40];
    std::snprintf(file_name, sizeof(file_name), "vp-%016llX.cso", static_cast<unsigned long long>(hash));
    const std::filesystem::path file = program_cache_folder() / file_name;
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
        if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &staging))) return false;
    }
    const D3D11_BOX box{x * g_scale, y * g_scale, 0, x * g_scale + 1, y * g_scale + 1, 1};
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
        const D3D11_BOX box{b.source_x * g_scale, b.source_y * g_scale, 0, (b.source_x + b.width) * g_scale,
                            (b.source_y + b.height) * g_scale, 1};
        g_context->CopySubresourceRegion(to->texture.Get(), 0, b.destination_x * g_scale, b.destination_y * g_scale, 0,
                                         from.texture.Get(), 0, &box);
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

void renderer_toggle_effects() {
    g_effects_enabled = !g_effects_enabled;
    log_line("renderer: modern effects %s", g_effects_enabled ? "on" : "off");
}

void renderer_present(uint32_t address, uint32_t width, uint32_t height, uint32_t pitch) {
    if (!g_device || !width || !height) return;
    ID3D11Texture2D* frame = nullptr;
    ID3D11ShaderResourceView* frame_view = nullptr;
    const auto target = g_targets.find(address);
    if (target != g_targets.end()) {
        frame = target->second.texture.Get();
        frame_view = target->second.srv.Get();
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
    present_frame(frame_view, frame_desc.Width, frame_desc.Height);
    ++g_presented;
    ++g_stats.presents;
    log_statistics();
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
            next_timed_dump += static_cast<ULONGLONG>(dump_seconds * 1000);
        }
    }
    if ((g_dump_every && g_presented % g_dump_every == 0) || timed_dump) {
        dump_frame(frame);  // the game's frame at its own size, not the window
        static const bool dump_window = GetEnvironmentVariableA("MC3_DUMP_WINDOW", nullptr, 0) != 0;
        if (dump_window) {  // what the window shows, with the effects (window-NNNNN.bmp)
            ComPtr<ID3D11Texture2D> back_buffer;
            g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
            dump_frame(back_buffer.Get(), "window");
        }
        const auto extra = g_targets.find(g_dump_target);
        if (extra != g_targets.end()) dump_frame(extra->second.texture.Get(), "target");
    }
    g_swap_chain->Present(0, 0);
}

}  // namespace mc3::pc
