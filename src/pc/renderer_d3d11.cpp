#include "pc/renderer_d3d11.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <tuple>
#include <unordered_map>

#include "pc/log.h"
#include "pc/physical_memory.h"

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
PSInput main(VSInput i) {
    PSInput o;
    o.position = i.position;
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

struct Target {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    uint32_t width = 0, height = 0;
};

struct DepthTarget {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11DepthStencilView> dsv;
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
ComPtr<ID3D11Buffer> g_vertex_buffer;
uint32_t g_vertex_capacity = 0;
ComPtr<ID3D11Buffer> g_constants;
ComPtr<ID3D11RasterizerState> g_rasterizer;
ComPtr<ID3D11Texture2D> g_upload;  // guest framebuffers that were never rendered to
uint32_t g_upload_width = 0, g_upload_height = 0;

std::unordered_map<uint32_t, Target> g_targets;
std::unordered_map<uint64_t, DepthTarget> g_depth_targets;
std::unordered_map<uint64_t, ComPtr<ID3D11PixelShader>> g_pixel_shaders;
std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>, CachedTexture> g_textures;
std::map<std::tuple<bool, uint32_t, uint32_t, uint32_t, uint32_t>, ComPtr<ID3D11BlendState>> g_blend_states;
std::map<std::tuple<bool, uint32_t, bool>, ComPtr<ID3D11DepthStencilState>> g_depth_states;
std::map<std::pair<uint32_t, uint32_t>, ComPtr<ID3D11SamplerState>> g_samplers;

// Statistics, logged every 10 s from renderer_present: where the GPU thread's time goes.
struct Statistics {
    uint64_t presents = 0, draws = 0, vertices = 0, vertex_ticks = 0;
    uint64_t hashed_bytes = 0, hash_ticks = 0, uploads = 0, upload_ticks = 0, draw_ticks = 0;
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

// The surface at `address`, at least width x height. A smaller request reuses the existing target (the draw's
// viewport covers only its part); a larger one grows it and keeps what was drawn so far.
Target* get_target(uint32_t address, uint32_t width, uint32_t height) {
    Target& target = g_targets[address];
    if (target.texture && target.width >= width && target.height >= height) return &target;
    width = std::max(width, target.width);
    height = std::max(height, target.height);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Target grown{};
    if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &grown.texture))) return nullptr;
    g_device->CreateRenderTargetView(grown.texture.Get(), nullptr, &grown.rtv);
    g_device->CreateShaderResourceView(grown.texture.Get(), nullptr, &grown.srv);
    grown.width = width;
    grown.height = height;
    if (target.texture) {
        const D3D11_BOX box{0, 0, 0, target.width, target.height, 1};
        g_context->CopySubresourceRegion(grown.texture.Get(), 0, 0, 0, 0, target.texture.Get(), 0, &box);
    }
    target = grown;
    log_line("renderer: render target %ux%u at %08X", width, height, address);
    return &target;
}

// Depth buffers must match their render target's size exactly in D3D11: one per address and size.
DepthTarget* get_depth_target(uint32_t address, uint32_t width, uint32_t height) {
    DepthTarget& target = g_depth_targets[(static_cast<uint64_t>(width) << 48) | (static_cast<uint64_t>(height) << 32) |
                                          address];
    if (target.texture && target.width == width && target.height == height) return &target;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    target = DepthTarget{};
    if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &target.texture))) return nullptr;
    g_device->CreateDepthStencilView(target.texture.Get(), nullptr, &target.dsv);
    target.width = width;
    target.height = height;
    return &target;
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

ID3D11DepthStencilState* depth_state(const DrawState& state) {
    const auto key = std::make_tuple(state.depth_test, state.depth_function, state.depth_write);
    ComPtr<ID3D11DepthStencilState>& cached = g_depth_states[key];
    if (cached) return cached.Get();
    D3D11_DEPTH_STENCIL_DESC desc{};
    desc.DepthEnable = state.depth_test || state.depth_write;
    desc.DepthWriteMask = state.depth_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    desc.DepthFunc = state.depth_test ? static_cast<D3D11_COMPARISON_FUNC>((state.depth_function & 7) + 1)
                                      : D3D11_COMPARISON_ALWAYS;
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
    const bool linear_mag = ((stage.filter >> 24) & 0xF) != 1;
    const bool linear_min = ((stage.filter >> 16) & 0xFF) != 1;
    desc.Filter = linear_mag ? (linear_min ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT)
                             : (linear_min ? D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_POINT);
    desc.AddressU = address_mode(stage.address_mode & 0xF);
    desc.AddressV = address_mode((stage.address_mode >> 8) & 0xF);
    desc.AddressW = address_mode((stage.address_mode >> 16) & 0xF);
    desc.MaxLOD = D3D11_FLOAT32_MAX;
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
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = image.width;
    desc.Height = image.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = image.layout == TextureLayout::Bc1   ? DXGI_FORMAT_BC1_UNORM
                  : image.layout == TextureLayout::Bc2 ? DXGI_FORMAT_BC2_UNORM
                  : image.layout == TextureLayout::Bc3 ? DXGI_FORMAT_BC3_UNORM
                                                       : DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{image.data.data(), image.row_pitch, 0};
    cached = CachedTexture{};
    if (FAILED(g_device->CreateTexture2D(&desc, &data, &cached.texture))) return nullptr;
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

void ensure_swap_chain_size(uint32_t width, uint32_t height) {
    if (width == g_swap_width && height == g_swap_height) return;
    g_swap_chain->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    g_swap_width = width;
    g_swap_height = height;
}

}  // namespace

bool renderer_start(HWND window) {
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
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);

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

    D3D11_BUFFER_DESC constants{};
    constants.ByteWidth = sizeof(CombinerConstants);
    constants.Usage = D3D11_USAGE_DEFAULT;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_device->CreateBuffer(&constants, nullptr, &g_constants);

    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;  // winding is not translated yet: draw both faces
    raster.DepthClipEnable = TRUE;
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
    if (flags & 0xF0) {
        if (Target* target = get_target(surface.color_address, surface.width, surface.height)) {
            float rgba[4];
            unpack_color(color, rgba);
            const D3D11_RECT rect{x0, y0, x1 + 1, y1 + 1};
            g_context1->ClearView(target->rtv.Get(), rgba, &rect, 1);
        }
    }
    if ((flags & 0x3) && surface.zeta_address) {
        if (DepthTarget* depth = get_depth_target(surface.zeta_address, surface.width, surface.height)) {
            const float z = static_cast<float>(depth_stencil >> 8) / 16777215.0f;
            g_context->ClearDepthStencilView(depth->dsv.Get(),
                                             ((flags & 1) ? D3D11_CLEAR_DEPTH : 0) | ((flags & 2) ? D3D11_CLEAR_STENCIL : 0),
                                             z, static_cast<UINT8>(depth_stencil));
        }
    }
}

void renderer_draw(const SurfaceState& surface, const DrawState& state, const std::vector<RenderVertex>& triangles) {
    if (!g_device || triangles.empty() || !surface.width || !surface.height) return;
    ++g_stats.draws;
    const uint64_t draw_start = ticks();
    struct Account {
        uint64_t start;
        ~Account() { g_stats.draw_ticks += ticks() - start; }
    } account{draw_start};
    Target* target = get_target(surface.color_address, surface.width, surface.height);
    if (!target) return;
    ID3D11PixelShader* shader = pixel_shader(state.program);
    if (!shader) return;
    DepthTarget* depth = (surface.zeta_address && (state.depth_test || state.depth_write))
                             ? get_depth_target(surface.zeta_address, target->width, target->height)
                             : nullptr;

    const uint32_t bytes = static_cast<uint32_t>(triangles.size() * sizeof(RenderVertex));
    if (bytes > g_vertex_capacity) {
        g_vertex_capacity = std::max(bytes, g_vertex_capacity * 2);
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = g_vertex_capacity;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        g_vertex_buffer.Reset();
        g_device->CreateBuffer(&desc, nullptr, &g_vertex_buffer);
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(g_context->Map(g_vertex_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    std::memcpy(mapped.pData, triangles.data(), bytes);
    g_context->Unmap(g_vertex_buffer.Get(), 0);

    ID3D11RenderTargetView* rtv = target->rtv.Get();
    // A surface can be both the target and a texture of the same draw; unbind it from the pixel shader first.
    ID3D11ShaderResourceView* none[4] = {};
    g_context->PSSetShaderResources(0, 4, none);
    g_context->OMSetRenderTargets(1, &rtv, depth ? depth->dsv.Get() : nullptr);
    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(surface.width), static_cast<float>(surface.height), 0, 1};
    g_context->RSSetViewports(1, &viewport);
    g_context->RSSetState(g_rasterizer.Get());
    g_context->OMSetBlendState(blend_state(state), state.blend_color, 0xFFFFFFFF);
    g_context->OMSetDepthStencilState(depth_state(state), 0);

    g_context->UpdateSubresource(g_constants.Get(), 0, nullptr, &state.constants, 0, 0);
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

    const UINT stride = sizeof(RenderVertex), offset = 0;
    ID3D11Buffer* buffer = g_vertex_buffer.Get();
    g_context->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
    g_context->IASetInputLayout(g_input_layout.Get());
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_vertex_shader.Get(), nullptr, 0);
    g_context->PSSetShader(shader, nullptr, 0);
    g_context->Draw(static_cast<UINT>(triangles.size()), 0);
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
        D3D11_BOX box{b.source_x, b.source_y, 0, b.source_x + b.width, b.source_y + b.height, 1};
        g_context->CopySubresourceRegion(to->texture.Get(), 0, b.destination_x, b.destination_y, 0, from.texture.Get(),
                                         0, &box);
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
        D3D11_BOX box{b.destination_x, b.destination_y, 0, b.destination_x + b.width, b.destination_y + b.height, 1};
        g_context->UpdateSubresource(to.texture.Get(), 0, &box,
                                     physical_pointer(b.destination + b.destination_y * b.destination_pitch +
                                                      b.destination_x * 4),
                                     b.destination_pitch, 0);
    }
}

void renderer_present(uint32_t address, uint32_t width, uint32_t height, uint32_t pitch) {
    if (!g_device || !width || !height) return;
    ComPtr<ID3D11Texture2D> back_buffer;
    const auto target = g_targets.find(address);
    if (target != g_targets.end()) {
        ensure_swap_chain_size(target->second.width, target->second.height);
        g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
        g_context->CopyResource(back_buffer.Get(), target->second.texture.Get());
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
            g_device->CreateTexture2D(&desc, nullptr, &g_upload);
            g_upload_width = width;
            g_upload_height = height;
        }
        g_context->UpdateSubresource(g_upload.Get(), 0, nullptr, pixels, pitch, 0);
        ensure_swap_chain_size(width, height);
        g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
        g_context->CopyResource(back_buffer.Get(), g_upload.Get());
    }
    ++g_presented;
    ++g_stats.presents;
    log_statistics();
    if (g_dump_every && g_presented % g_dump_every == 0) {
        dump_frame(back_buffer.Get());
        const auto extra = g_targets.find(g_dump_target);
        if (extra != g_targets.end()) dump_frame(extra->second.texture.Get(), "target");
    }
    g_swap_chain->Present(0, 0);
}

}  // namespace mc3::pc
