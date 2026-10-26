#include "pc/launcher_ui.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <urlmon.h>
#include <winhttp.h>
#include <wrl/client.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../../res/launcher_resources.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "pc/keyboard_bindings.h"
#include "pc/launcher_art.h"
#include "pc/launcher_music.h"
#include "pc/log.h"
#include "pc/nv2a.h"
#include "pc/paths.h"
#include "pc/settings.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

#ifndef MC3PORT_VERSION
#define MC3PORT_VERSION "0.0.0"
#endif

namespace mc3::pc {
namespace {

using Microsoft::WRL::ComPtr;

constexpr char kRepository[] = "SickSartori/mc3port";

// --- texts: the launcher speaks Windows' language (Italian, else English) ---

struct Texts {
    const char *play, *settings, *controls, *about, *quit;
    const char *ready, *ready_lead, *disc_ok, *disc_none, *disc_none_sub, *change, *choose_iso;
    const char *pad_on, *pad_on_sub, *pad_off, *pad_off_sub, *edit;
    const char *tab_game, *tab_display, *tab_graphics, *tab_effects;
    const char *game_language, *game_language_sub, *like_windows, *fps, *fps_sub, *volume, *music_volume, *music_volume_sub;
    const char *aspect, *aspect_sub, *window_mode, *window_mode_sub, *windowed, *fullscreen;
    const char *quality, *quality_sub, *original, *balanced, *high, *custom, *scale, *scale_sub;
    const char *anisotropy, *gpu_vertices, *gpu_vertices_sub, *effects, *effects_sub, *on, *off;
    const char *sharpen, *bloom, *bloom_threshold, *saturation, *contrast, *vignette, *occlusion, *occlusion_radius;
    const char *defaults, *controls_lead, *press_key, *reset_keys, *system_keys, *full_screen_key, *effects_key, *launcher_keys, *pads_note;
    const char *about_lead, *checking, *up_to_date, *update_found, *update_sub, *open, *logs, *logs_sub, *legal, *credits;
    const char *update_now, *update_install_sub, *downloading, *update_failed;
    const char *welcome, *welcome_lead, *next, *quality_title, *quality_lead, *all_set, *all_set_lead;
    const char *bar_disc, *bar_disc_ok, *bar_disc_none, *bar_pad, *bar_pad_on, *bar_pad_off, *bar_version, *select, *back;
};

const Texts kEnglish = {
    "PLAY", "SETTINGS", "CONTROLS", "ABOUT", "QUIT",
    "READY TO RACE", "The streets of San Diego, Atlanta, Detroit and Tokyo are waiting.", "Disc verified",
    "No disc chosen", "Choose the disc image (ISO) of your European Xbox copy.", "Change", "Choose the ISO",
    "Controller connected", "Xbox and compatible controllers", "No controller", "The keyboard works too", "Edit",
    "Game", "Display", "Graphics", "Effects",
    "Game language", "The game's text and speech", "Like Windows", "Frames per second", "60 is smoother; 30 like the Xbox",
    "Game volume", "Launcher music", "This screen's background music; 0 turns it off",
    "Aspect ratio", "16:9 uses the game's own widescreen mode", "Window mode", "Alt+Enter or F11 while playing",
    "Window", "Full screen",
    "Quality", "Sets resolution, filtering and effects together", "Original", "Balanced", "High", "Custom",
    "Internal resolution", "Smoother edges, more graphics card work",
    "Anisotropic filtering", "Vertices on the graphics card", "Faster; off only for troubleshooting",
    "Modern effects", "Bloom, sharpening, colour and shading (F10 while playing)", "On", "Off",
    "Sharpening", "Light glow", "Glow threshold", "Saturation", "Contrast", "Vignette", "Ambient occlusion",
    "Occlusion size",
    "Restore defaults", "Xbox controllers work as they are. Choose the keyboard keys here.", "Press a key...",
    "Restore keys", "PC KEYS", "Full screen on and off", "Modern effects on and off", "Launcher: move, select, back",
    "PlayStation and other pads: through Steam Input or DS4Windows, which present them as Xbox controllers.",
    "Unofficial native Windows port. No game files included: you need your own copy.", "Looking for updates...",
    "This is the latest version", "Version %s is available", "Opens the release page; nothing downloads by itself",
    "Open", "Log folder", "For bug reports, attach mc3port.log",
    "Midnight Club is a trademark of Take-Two Interactive / Rockstar Games. This project is not affiliated with them.",
    "MIT licence \xC2\xB7 Dear ImGui (MIT) \xC2\xB7 Barlow Semi Condensed (OFL)",
    "Update", "Downloads, checks and installs it; the game reopens by itself", "Downloading the update...",
    "The update could not be downloaded or checked",
    "WELCOME", "You need your own copy: a disc image (ISO) of the European Xbox Midnight Club 3: DUB Edition Remix.",
    "NEXT", "QUALITY", "You can change it any time in the settings.", "ALL SET",
    "Disc verified. Connect a controller or use the keyboard.",
    "Disc", "Xbox \xC2\xB7 Europe", "none", "Controller", "connected", "not connected", "Version", "Select", "Back",
};

const Texts kItalian = {
    "GIOCA", "IMPOSTAZIONI", "CONTROLLI", "INFO", "ESCI",
    "PRONTO A CORRERE", "Le strade di San Diego, Atlanta, Detroit e Tokyo ti aspettano.", "Disco verificato",
    "Nessun disco scelto", "Scegli l'immagine del disco (ISO) della tua copia Xbox europea.", "Cambia", "Scegli l'ISO",
    "Controller collegato", "Controller Xbox e compatibili", "Nessun controller", "Anche la tastiera funziona", "Modifica",
    "Gioco", "Schermo", "Grafica", "Effetti",
    "Lingua del gioco", "Testi e voci del gioco", "Come Windows", "Fotogrammi al secondo",
    "60 \xC3\xA8 pi\xC3\xB9 fluido; 30 come l'Xbox", "Volume del gioco", "Musica del launcher",
    "La musica di sottofondo di questa schermata; 0 la spegne",
    "Formato", "16:9 usa la modalit\xC3\xA0 panoramica del gioco", "Modalit\xC3\xA0 finestra",
    "Alt+Invio o F11 durante il gioco", "Finestra", "Schermo intero",
    "Qualit\xC3\xA0", "Imposta insieme risoluzione, filtri ed effetti", "Originale", "Bilanciata", "Alta",
    "Personalizzata", "Risoluzione interna", "Bordi pi\xC3\xB9 lisci, pi\xC3\xB9 lavoro per la scheda video",
    "Filtro anisotropico", "Vertici sulla scheda video", "Pi\xC3\xB9 veloce; da spegnere solo in caso di problemi",
    "Effetti moderni", "Bagliore, nitidezza, colori e ombreggiatura (F10 durante il gioco)", "S\xC3\xAC", "No",
    "Nitidezza", "Bagliore delle luci", "Soglia del bagliore", "Saturazione", "Contrasto", "Vignettatura",
    "Ombreggiatura ambientale", "Ampiezza dell'ombreggiatura",
    "Valori predefiniti", "I controller Xbox funzionano cos\xC3\xAC come sono. Qui scegli i tasti della tastiera.",
    "Premi un tasto...", "Tasti predefiniti", "TASTI DEL PC", "Schermo intero s\xC3\xAC/no", "Effetti moderni s\xC3\xAC/no",
    "Launcher: muoversi, scegliere, tornare",
    "Controller PlayStation e altri: tramite Steam Input o DS4Windows, che li presentano come controller Xbox.",
    "Port nativo non ufficiale per Windows. Nessun file del gioco incluso: serve la tua copia.",
    "Cerco aggiornamenti...", "Questa \xC3\xA8 l'ultima versione", "\xC3\x88 disponibile la versione %s",
    "Apre la pagina della versione; niente si scarica da solo", "Apri", "Cartella dei log",
    "Per segnalare un problema allega mc3port.log",
    "Midnight Club \xC3\xA8 un marchio di Take-Two Interactive / Rockstar Games. Progetto non affiliato.",
    "Licenza MIT \xC2\xB7 Dear ImGui (MIT) \xC2\xB7 Barlow Semi Condensed (OFL)",
    "Aggiorna", "Si scarica, si verifica e si installa; il gioco si riapre da solo", "Scarico l'aggiornamento...",
    "Non \xC3\xA8 stato possibile scaricare o verificare l'aggiornamento",
    "BENVENUTO",
    "Per giocare serve la tua copia: l'immagine ISO del disco Xbox europeo di Midnight Club 3: DUB Edition Remix.",
    "AVANTI", "QUALIT\xC3\x80", "Potrai cambiarla quando vuoi nelle impostazioni.", "TUTTO PRONTO",
    "Disco verificato. Collega un controller o usa la tastiera.",
    "Disco", "Xbox \xC2\xB7 Europa", "nessuno", "Controller", "collegato", "non collegato", "Versione", "Seleziona",
    "Indietro",
};

const Texts& T() {
    static const Texts& texts = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_ITALIAN ? kItalian : kEnglish;
    return texts;
}
bool italian() { return &T() == &kItalian; }

// --- colours ---

constexpr ImU32 kRed = IM_COL32(227, 38, 47, 255), kRedDark = IM_COL32(158, 13, 21, 255);
constexpr ImU32 kInk = IM_COL32(242, 241, 238, 255), kDim = IM_COL32(163, 163, 168, 255);
constexpr ImU32 kGreen = IM_COL32(110, 227, 110, 255), kAmber = IM_COL32(255, 178, 30, 255);
constexpr ImU32 kChrome = IM_COL32(217, 218, 223, 255);

float g_alpha = 1;  // the current element's fade

ImU32 fade(ImU32 colour, float alpha = 1) {
    const float a = static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha * g_alpha, 0.0f, 1.0f);
    return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}
ImU32 mix(ImU32 a, ImU32 b, float t) {
    return ImGui::ColorConvertFloat4ToU32(ImLerp(ImGui::ColorConvertU32ToFloat4(a), ImGui::ColorConvertU32ToFloat4(b), t));
}
float ease_out(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

// --- shared state with the window thread ---

std::mutex g_ui_mutex;                 // ImGui's input queue is filled by the window thread
std::atomic<int> g_captured_key{0};    // the key pressed while rebinding (0: none)
std::atomic<bool> g_capturing{false};

int key_from_message(WPARAM wparam, LPARAM lparam) {
    const UINT scan = (lparam >> 16) & 0xFF;
    const bool extended = (lparam >> 24) & 1;
    switch (wparam) {
        case VK_SHIFT: return static_cast<int>(MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX));
        case VK_CONTROL: return extended ? VK_RCONTROL : VK_LCONTROL;
        case VK_MENU: return extended ? VK_RMENU : VK_LMENU;
        default: return static_cast<int>(wparam);
    }
}

bool window_hook(HWND window, UINT message, WPARAM wparam, LPARAM lparam, LRESULT* result) {
    if (g_capturing && (message == WM_KEYDOWN || message == WM_SYSKEYDOWN)) {
        g_captured_key = key_from_message(wparam, lparam);
        g_capturing = false;
        *result = 0;
        return true;
    }
    std::lock_guard<std::mutex> lock(g_ui_mutex);
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) {
        *result = 1;
        return true;
    }
    return false;
}

std::string key_name(int virtual_key) {
    UINT scan = MapVirtualKeyW(static_cast<UINT>(virtual_key), MAPVK_VK_TO_VSC_EX);
    switch (virtual_key) {
        case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT: case VK_INSERT: case VK_DELETE: case VK_HOME:
        case VK_END: case VK_PRIOR: case VK_NEXT: case VK_RCONTROL: case VK_RMENU: case VK_DIVIDE: case VK_NUMLOCK:
            scan |= 0x100;
            break;
        default: break;
    }
    wchar_t name[64] = {};
    const int length = GetKeyNameTextW(static_cast<LONG>(((scan & 0xFF) << 16) | ((scan & 0x100) ? (1 << 24) : 0)), name, 64);
    if (length <= 0) return "?";
    char utf8[192] = {};
    WideCharToMultiByte(CP_UTF8, 0, name, length, utf8, sizeof(utf8) - 1, nullptr, nullptr);
    return utf8;
}

// --- update check (background thread) ---

struct UpdateCheck {
    std::mutex mutex;
    int state = 0;  // 0 checking, 1 up to date, 2 newer found, 3 failed (shown as nothing)
    std::string version, url;
    std::string setup_url, sums_url;  // the release's installer and SHA256SUMS, if it has them
    int download = 0;                 // 0 none, 1 downloading, 2 verified (start it), 3 failed
    std::filesystem::path setup_file;
};

std::vector<int> version_numbers(const std::string& text) {
    std::vector<int> numbers;
    for (size_t i = 0; i < text.size();) {
        if (std::isdigit(static_cast<unsigned char>(text[i]))) {
            size_t end = i;
            while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) ++end;
            numbers.push_back(std::atoi(text.substr(i, end - i).c_str()));
            i = end;
        } else {
            ++i;
        }
    }
    return numbers;
}

std::string json_string(const std::string& json, const char* key) {
    const std::string marker = std::string("\"") + key + "\":\"";
    const size_t at = json.find(marker);
    if (at == std::string::npos) return {};
    const size_t start = at + marker.size(), end = json.find('"', start);
    return end == std::string::npos ? std::string() : json.substr(start, end - start);
}

void check_for_updates(UpdateCheck* check) {
    std::string body;
    const std::string version = MC3PORT_VERSION;
    const std::wstring agent = L"mc3port/" + std::wstring(version.begin(), version.end());
    HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    HINTERNET connection = session ? WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
    wchar_t path[128];
    std::swprintf(path, 128, L"/repos/%hs/releases/latest", kRepository);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path, nullptr, nullptr, nullptr,
                                                        WINHTTP_FLAG_SECURE)
                                   : nullptr;
    DWORD status = 0, size = sizeof(status);
    if (request && WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n", static_cast<DWORD>(-1),
                                      nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr) &&
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size,
                            nullptr) &&
        status == 200) {
        char buffer[4096];
        DWORD read = 0;
        while (WinHttpReadData(request, buffer, sizeof(buffer), &read) && read) body.append(buffer, read);
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    const std::string tag = json_string(body, "tag_name"), url = json_string(body, "html_url");
    std::lock_guard<std::mutex> lock(check->mutex);
    if (tag.empty()) {
        check->state = 3;
        log_line("launcher: update check: no answer (HTTP %lu)", status);
        return;
    }
    const bool newer = version_numbers(tag) > version_numbers(MC3PORT_VERSION);
    check->state = newer ? 2 : 1;
    check->version = tag;
    check->url = url;
    const std::string marker = "\"browser_download_url\":\"";
    for (size_t at = body.find(marker); at != std::string::npos; at = body.find(marker, at + 1)) {
        const size_t start = at + marker.size(), end = body.find('"', start);
        const std::string asset = body.substr(start, end - start);
        if (asset.size() > 10 && asset.compare(asset.size() - 10, 10, "-setup.exe") == 0) check->setup_url = asset;
        if (asset.size() > 10 && asset.compare(asset.size() - 10, 10, "SHA256SUMS") == 0) check->sums_url = asset;
    }
    log_line("launcher: latest release %s (%s)", tag.c_str(), newer ? "newer" : "not newer");
}

std::string file_sha256(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    uint8_t digest[32] = {};
    if (data.empty() || BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                                   static_cast<ULONG>(data.size()), digest, sizeof(digest)) != 0) {
        return {};
    }
    std::string hex;
    char byte[3];
    for (uint8_t b : digest) {
        std::snprintf(byte, sizeof(byte), "%02x", b);
        hex += byte;
    }
    return hex;
}

// Downloads the release's installer and SHA256SUMS to %TEMP%\mc3port-update and checks the installer's hash.
void download_update(UpdateCheck* check) {
    std::string setup_url, sums_url;
    {
        std::lock_guard<std::mutex> lock(check->mutex);
        setup_url = check->setup_url;
        sums_url = check->sums_url;
    }
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    const std::filesystem::path folder = std::filesystem::path(temp) / "mc3port-update";
    std::error_code ignored;
    std::filesystem::create_directories(folder, ignored);
    const std::string name = setup_url.substr(setup_url.rfind('/') + 1);
    const std::filesystem::path setup = folder / name, sums = folder / "SHA256SUMS";
    const std::wstring setup_wide(setup_url.begin(), setup_url.end()), sums_wide(sums_url.begin(), sums_url.end());
    bool good = !sums_url.empty() && SUCCEEDED(URLDownloadToFileW(nullptr, setup_wide.c_str(), setup.c_str(), 0, nullptr)) &&
                SUCCEEDED(URLDownloadToFileW(nullptr, sums_wide.c_str(), sums.c_str(), 0, nullptr));
    if (good) {
        std::ifstream list(sums);
        const std::string hash = file_sha256(setup);
        good = false;
        for (std::string line; std::getline(list, line);) {
            if (!hash.empty() && line.size() > 66 && line.compare(66, std::string::npos, name) == 0 && line.compare(0, 64, hash) == 0) {
                good = true;
            }
        }
    }
    log_line("launcher: update %s %s", name.c_str(), good ? "downloaded and verified" : "could not be downloaded or verified");
    std::lock_guard<std::mutex> lock(check->mutex);
    check->download = good ? 2 : 3;
    check->setup_file = setup;
}

// --- the background: the disc's artwork, crossfading with a slow zoom, graded towards the game's menus ---

constexpr char kBackgroundShader[] = R"(
cbuffer Background : register(b0) {
    float4 a_rect;   // uv offset (xy) and scale (zw) of picture A
    float4 b_rect;
    float4 params;   // x: weight of B, y: 1 with pictures, z: time, w: screen aspect
};
Texture2D picture_a : register(t0);
Texture2D picture_b : register(t1);
SamplerState linear_clamp : register(s0);
struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Output vs(uint id : SV_VertexID) {
    Output o;
    const float2 t = float2((id << 1) & 2, id & 2);
    o.position = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}
float3 grade(float3 c) {
    const float luma = dot(c, float3(0.299, 0.587, 0.114));
    c = lerp(luma.xxx, c, 0.45);
    return saturate((c - 0.5) * 1.1 + 0.5) * 0.95;
}
float4 ps(Output i) : SV_Target {
    const float2 uv = i.uv;
    float3 c;
    if (params.y > 0.5) {
        const float3 a = grade(picture_a.Sample(linear_clamp, a_rect.xy + uv * a_rect.zw).rgb);
        const float3 b = grade(picture_b.Sample(linear_clamp, b_rect.xy + uv * b_rect.zw).rgb);
        c = lerp(a, b, params.x);
    } else {
        c = lerp(float3(0.03, 0.03, 0.035), float3(0.085, 0.08, 0.08), uv.y);
        const float2 d = (uv - float2(0.74, 0.86)) * float2(params.w, 1);
        c += float3(0.42, 0.035, 0.045) * exp(-dot(d, d) * 5.0) * (0.85 + 0.15 * sin(params.z * 0.7));
    }
    const float3 ink = float3(0.024, 0.024, 0.027);
    float left = 1 - smoothstep(0.0, 0.66, uv.x);
    c = lerp(c, ink, 0.3 + 0.67 * left);
    c = lerp(c, ink, (1 - smoothstep(0.0, 0.3, 1 - uv.y)) * 0.92);
    const float2 v = (uv - 0.5) * float2(params.w / 1.78, 1);
    c *= 1 - 0.35 * smoothstep(0.35, 0.9, length(v));
    return float4(c, 1);
}
)";

struct BackgroundConstants {
    float a_rect[4], b_rect[4], params[4];
};

// --- the launcher ---

struct Trail {
    float x, y, speed, length;
    ImU32 colour;
};

class Launcher {
public:
    Launcher(HWND window, const std::filesystem::path& exe_directory) : window_(window), exe_directory_(exe_directory) {}
    std::optional<GameDisc> run();

private:
    enum class Page { kPlay, kSettings, kControls, kAbout, kQuit };

    bool start_graphics();
    void stop_graphics();
    void load_fonts();
    void resize_if_needed();
    void upload_art();
    void start_art_loader(const std::filesystem::path& disc_path);
    void draw_background(float now);
    void frame(float now, float dt);

    // Layout helpers: design pixels (1600x900) times scale_.
    float px(float value) const { return value * scale_; }
    void text(ImFont* font, float size, ImVec2 at, ImU32 colour, const char* str, float wrap = 0);
    ImVec2 measure(ImFont* font, float size, const char* str, float wrap = 0) const;
    void skewed_rect(ImDrawList* list, ImVec2 a, ImVec2 b, ImU32 left, ImU32 right, float skew_ratio = 0.21f);
    float anim(ImGuiID id, bool on, float speed = 12.0f);

    // Panel content: cursor_ moves down.
    void title(const char* str);
    void lead(const char* str);
    bool status(bool good, const char* heading, const char* detail, const char* button_label, const char* id);
    bool small_button(const char* label, ImVec2 at, const char* id, ImVec2* size_out = nullptr);
    bool big_button(const char* label, const char* id, bool pulse);
    bool tabs(const char* const* labels, int count, int* current);
    void row(const char* heading, const char* detail);
    bool segmented(const char* id, const char* const* labels, int count, int* current);
    bool slider(const char* id, float* value, float minimum, float maximum, const char* format);
    void steps(int current, int total);

    void page_play();
    void page_settings();
    void page_controls();
    void page_about();
    void update_row(bool always);
    void page_wizard();
    void choose_disc();
    void set_setting(const char* key, float value) { setting_save(key, value); }
    int quality_preset() const;
    void apply_quality(int preset);

    HWND window_;
    std::filesystem::path exe_directory_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swap_chain_;
    ComPtr<ID3D11RenderTargetView> target_;
    ComPtr<ID3D11VertexShader> background_vs_;
    ComPtr<ID3D11PixelShader> background_ps_;
    ComPtr<ID3D11Buffer> background_constants_;
    ComPtr<ID3D11SamplerState> sampler_;
    UINT width_ = 0, height_ = 0;
    float scale_ = 1;

    ImFont *body_ = nullptr, *body_bold_ = nullptr, *italic_ = nullptr, *heading_ = nullptr, *black_ = nullptr;
    std::vector<std::pair<int, std::vector<char>>> font_data_;

    std::optional<GameDisc> disc_;
    std::string disc_problem_;
    std::mutex art_mutex_;
    std::optional<LauncherArt> loaded_art_;  // from the loader thread, uploaded by the next frame
    std::thread art_thread_;
    ComPtr<ID3D11ShaderResourceView> logo_;
    std::vector<ComPtr<ID3D11ShaderResourceView>> artwork_;
    int slide_ = 0;
    float slide_start_ = -100;

    std::unique_ptr<LauncherMusic> music_;
    UpdateCheck updates_;
    std::thread update_thread_, download_thread_;
    HMODULE xinput_ = nullptr;
    decltype(&XInputGetState) xinput_get_state_ = nullptr;
    bool pad_connected_ = false;
    float pad_checked_ = -10;

    Page page_ = Page::kPlay;
    float page_since_ = 0;
    int settings_tab_ = 0;
    bool wizard_ = false;
    int wizard_step_ = 0;
    int rebinding_ = -1;
    float rebinding_since_ = 0;
    bool done_ = false, play_ = false;
    float start_time_ = 0;

    ImDrawList* list_ = nullptr;
    ImVec2 origin_;  // panel content top left
    float panel_width_ = 0, cursor_ = 0;
    std::unordered_map<ImGuiID, float> anims_;
    std::vector<Trail> trails_;
    std::vector<ImVec4> drops_;
};

std::vector<char> load_font_resource(int id) {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&load_font_resource), &module);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL handle = resource ? LoadResource(module, resource) : nullptr;
    const char* data = handle ? static_cast<const char*>(LockResource(handle)) : nullptr;
    return data ? std::vector<char>(data, data + SizeofResource(module, resource)) : std::vector<char>();
}

bool Launcher::start_graphics() {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                 D3D11_SDK_VERSION, &device_, nullptr, &context_))) {
        return false;
    }
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    device_.As(&dxgi_device);
    if (!dxgi_device || FAILED(dxgi_device->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        return false;
    }
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(factory->CreateSwapChainForHwnd(device_.Get(), window_, &desc, nullptr, nullptr, &swap_chain_))) return false;
    factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER);

    ComPtr<ID3DBlob> vs, ps, errors;
    if (FAILED(D3DCompile(kBackgroundShader, sizeof(kBackgroundShader) - 1, "launcher background", nullptr, nullptr, "vs",
                          "vs_5_0", 0, 0, &vs, &errors)) ||
        FAILED(D3DCompile(kBackgroundShader, sizeof(kBackgroundShader) - 1, "launcher background", nullptr, nullptr, "ps",
                          "ps_5_0", 0, 0, &ps, &errors))) {
        log_line("launcher: background shader: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return false;
    }
    device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &background_vs_);
    device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &background_ps_);
    D3D11_BUFFER_DESC constants{};
    constants.ByteWidth = sizeof(BackgroundConstants);
    constants.Usage = D3D11_USAGE_DEFAULT;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    device_->CreateBuffer(&constants, nullptr, &background_constants_);
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    device_->CreateSamplerState(&sampler, &sampler_);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplWin32_Init(window_);
    ImGui_ImplDX11_Init(device_.Get(), context_.Get());
    load_fonts();

    ImGuiStyle& style = ImGui::GetStyle();
    style.FrameRounding = 0;
    style.FrameBorderSize = 0;
    style.PopupRounding = 0;
    style.GrabMinSize = 14;
    style.GrabRounding = 0;
    ImVec4* colours = style.Colors;
    colours[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(kInk);
    colours[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 0.07f);
    colours[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, 0.12f);
    colours[ImGuiCol_FrameBgActive] = ImVec4(1, 1, 1, 0.16f);
    colours[ImGuiCol_SliderGrab] = ImGui::ColorConvertU32ToFloat4(kRed);
    colours[ImGuiCol_SliderGrabActive] = ImVec4(1, 0.35f, 0.38f, 1);
    colours[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.07f, 0.08f, 0.97f);
    colours[ImGuiCol_Header] = ImGui::ColorConvertU32ToFloat4(kRed);
    colours[ImGuiCol_HeaderHovered] = ImVec4(0.89f, 0.15f, 0.18f, 0.7f);
    colours[ImGuiCol_HeaderActive] = ImGui::ColorConvertU32ToFloat4(kRedDark);
    colours[ImGuiCol_Button] = ImVec4(1, 1, 1, 0.07f);
    colours[ImGuiCol_ButtonHovered] = ImVec4(0.89f, 0.15f, 0.18f, 0.6f);
    colours[ImGuiCol_NavCursor] = ImVec4(1, 1, 1, 0.9f);
    colours[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.1f);

    xinput_ = LoadLibraryW(L"xinput1_4.dll");
    if (!xinput_) xinput_ = LoadLibraryW(L"xinput9_1_0.dll");
    if (xinput_) xinput_get_state_ = reinterpret_cast<decltype(&XInputGetState)>(GetProcAddress(xinput_, "XInputGetState"));
    return true;
}

void Launcher::load_fonts() {
    ImGuiIO& io = ImGui::GetIO();
    const auto add = [&](int id, bool symbols) -> ImFont* {
        font_data_.emplace_back(id, load_font_resource(id));
        std::vector<char>& data = font_data_.back().second;
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        ImFont* font = data.empty() ? io.Fonts->AddFontDefault()
                                    : io.Fonts->AddFontFromMemoryTTF(data.data(), static_cast<int>(data.size()), 20.0f, &config);
        if (symbols) {  // arrows and other signs Barlow lacks
            ImFontConfig merge;
            merge.MergeMode = true;
            io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisym.ttf", 20.0f, &merge);
        }
        return font;
    };
    body_ = add(IDR_FONT_MEDIUM, true);
    body_bold_ = add(IDR_FONT_SEMIBOLD, true);
    italic_ = add(IDR_FONT_MEDIUM_ITALIC, false);
    heading_ = add(IDR_FONT_EXTRABOLD_ITALIC, false);
    black_ = add(IDR_FONT_BLACK_ITALIC, false);
    io.FontDefault = body_;
}

void Launcher::stop_graphics() {
    music_.reset();  // fades out
    if (art_thread_.joinable()) art_thread_.join();
    if (update_thread_.joinable()) update_thread_.join();
    if (download_thread_.joinable()) download_thread_.join();
    {
        std::lock_guard<std::mutex> lock(g_ui_mutex);
        nv2a_set_window_hook(nullptr);
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    logo_.Reset();
    artwork_.clear();
    target_.Reset();
    if (context_) {
        context_->ClearState();
        context_->Flush();
    }
    swap_chain_.Reset();
    background_vs_.Reset();
    background_ps_.Reset();
    background_constants_.Reset();
    sampler_.Reset();
    context_.Reset();
    device_.Reset();
    if (xinput_) FreeLibrary(xinput_);
}

void Launcher::resize_if_needed() {
    RECT client{};
    GetClientRect(window_, &client);
    const UINT width = std::max<LONG>(1, client.right), height = std::max<LONG>(1, client.bottom);
    if (target_ && width == width_ && height == height_) return;
    target_.Reset();
    swap_chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    ComPtr<ID3D11Texture2D> buffer;
    swap_chain_->GetBuffer(0, IID_PPV_ARGS(&buffer));
    device_->CreateRenderTargetView(buffer.Get(), nullptr, &target_);
    width_ = width;
    height_ = height;
    // Everything scales with the window, designed on 1600x900; below 0.65 text would be too small to read (very
    // small windows crop).
    scale_ = std::max(0.65f, std::min(width / 1600.0f, height / 900.0f));
}

ComPtr<ID3D11ShaderResourceView> dxt_texture(ID3D11Device* device, const DxtImage& image) {
    if (image.blocks.empty()) return nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = image.width;
    desc.Height = image.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_BC3_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{image.blocks.data(), image.width / 4 * 16, 0};
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    if (SUCCEEDED(device->CreateTexture2D(&desc, &data, &texture))) device->CreateShaderResourceView(texture.Get(), nullptr, &view);
    return view;
}

void Launcher::start_art_loader(const std::filesystem::path& disc_path) {
    if (art_thread_.joinable()) art_thread_.join();
    art_thread_ = std::thread([this, disc_path] {
        std::string error;
        const std::unique_ptr<XdvdfsImage> disc = XdvdfsImage::open(disc_path, &error);
        if (!disc) return;
        LauncherArt art = load_launcher_art(*disc);
        std::lock_guard<std::mutex> lock(art_mutex_);
        loaded_art_ = std::move(art);
    });
}

void Launcher::upload_art() {
    std::optional<LauncherArt> art;
    {
        std::lock_guard<std::mutex> lock(art_mutex_);
        art.swap(loaded_art_);
    }
    if (!art) return;
    logo_ = dxt_texture(device_.Get(), art->logo);
    artwork_.clear();
    for (const DxtImage& picture : art->artwork) {
        if (ComPtr<ID3D11ShaderResourceView> view = dxt_texture(device_.Get(), picture)) artwork_.push_back(view);
    }
    slide_ = 0;
    slide_start_ = -100;
}

// The part of a 512x256 picture covering the screen: inside its painted frame, zoomed by `zoom`, panned by `pan`.
void cover_rect(float screen_aspect, float zoom, float pan, float* rect) {
    constexpr float kWidth = 512, kHeight = 256, kInnerWidth = kWidth * 0.94f, kInnerHeight = kHeight * 0.88f;
    float w, h;
    if (screen_aspect < kInnerWidth / kInnerHeight) {
        h = kInnerHeight / zoom;
        w = h * screen_aspect;
    } else {
        w = kInnerWidth / zoom;
        h = w / screen_aspect;
    }
    const float cx = std::clamp(kWidth * (0.5f - pan), kWidth * 0.03f + w / 2, kWidth * 0.97f - w / 2);
    const float cy = kHeight * 0.47f;
    rect[0] = (cx - w / 2) / kWidth;
    rect[1] = (cy - h / 2) / kHeight;
    rect[2] = w / kWidth;
    rect[3] = h / kHeight;
}

void Launcher::draw_background(float now) {
    constexpr float kSlide = 7.0f, kFade = 1.6f, kZoomTime = 9.0f;
    if (!artwork_.empty() && now - slide_start_ >= kSlide) {
        slide_ = slide_start_ < 0 ? 0 : (slide_ + 1) % static_cast<int>(artwork_.size());
        slide_start_ = now;
    }
    BackgroundConstants constants{};
    const float aspect = static_cast<float>(width_) / static_cast<float>(height_);
    ID3D11ShaderResourceView* views[2] = {};
    if (!artwork_.empty()) {
        const int previous = (slide_ + static_cast<int>(artwork_.size()) - 1) % static_cast<int>(artwork_.size());
        const float age = now - slide_start_;
        const float previous_age = age + kSlide;
        const float fade_in = artwork_.size() > 1 ? std::clamp(age / kFade, 0.0f, 1.0f) : 1.0f;
        cover_rect(aspect, 1.08f + 0.10f * std::min(previous_age / kZoomTime, 1.0f),
                   0.02f * std::min(previous_age / kZoomTime, 1.0f), constants.a_rect);
        cover_rect(aspect, 1.08f + 0.10f * std::min(age / kZoomTime, 1.0f), 0.02f * std::min(age / kZoomTime, 1.0f),
                   constants.b_rect);
        constants.params[0] = fade_in;
        constants.params[1] = 1;
        views[0] = artwork_[previous].Get();
        views[1] = artwork_[slide_].Get();
    }
    constants.params[2] = now;
    constants.params[3] = aspect;
    context_->UpdateSubresource(background_constants_.Get(), 0, nullptr, &constants, 0, 0);
    ID3D11RenderTargetView* target = target_.Get();
    context_->OMSetRenderTargets(1, &target, nullptr);
    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(background_vs_.Get(), nullptr, 0);
    context_->PSSetShader(background_ps_.Get(), nullptr, 0);
    ID3D11Buffer* buffer = background_constants_.Get();
    context_->PSSetConstantBuffers(0, 1, &buffer);
    context_->PSSetShaderResources(0, 2, views);
    ID3D11SamplerState* sampler = sampler_.Get();
    context_->PSSetSamplers(0, 1, &sampler);
    context_->Draw(3, 0);
}

// --- drawing helpers ---

void Launcher::text(ImFont* font, float size, ImVec2 at, ImU32 colour, const char* str, float wrap) {
    list_->AddText(font, size, at, fade(colour), str, nullptr, wrap);
}

ImVec2 Launcher::measure(ImFont* font, float size, const char* str, float wrap) const {
    return font->CalcTextSizeA(size, FLT_MAX, wrap, str);
}

// A parallelogram leaning right like the game's italic menus, coloured left to right.
void Launcher::skewed_rect(ImDrawList* list, ImVec2 a, ImVec2 b, ImU32 left, ImU32 right, float skew_ratio) {
    const float skew = (b.y - a.y) * skew_ratio;
    const ImVec2 uv = ImGui::GetDrawListSharedData()->TexUvWhitePixel;
    list->PrimReserve(6, 4);
    const ImDrawIdx index = static_cast<ImDrawIdx>(list->_VtxCurrentIdx);
    list->PrimWriteIdx(index);
    list->PrimWriteIdx(static_cast<ImDrawIdx>(index + 1));
    list->PrimWriteIdx(static_cast<ImDrawIdx>(index + 2));
    list->PrimWriteIdx(index);
    list->PrimWriteIdx(static_cast<ImDrawIdx>(index + 2));
    list->PrimWriteIdx(static_cast<ImDrawIdx>(index + 3));
    list->PrimWriteVtx(ImVec2(a.x + skew, a.y), uv, fade(left));
    list->PrimWriteVtx(ImVec2(b.x + skew, a.y), uv, fade(right));
    list->PrimWriteVtx(ImVec2(b.x, b.y), uv, fade(right));
    list->PrimWriteVtx(ImVec2(a.x, b.y), uv, fade(left));
}

float Launcher::anim(ImGuiID id, bool on, float speed) {
    float& value = anims_[id];
    value += ((on ? 1.0f : 0.0f) - value) * std::min(1.0f, ImGui::GetIO().DeltaTime * speed);
    return value;
}

void Launcher::title(const char* str) {
    text(black_, px(48), ImVec2(origin_.x, origin_.y + cursor_), kInk, str);
    cursor_ += px(56);
}

void Launcher::lead(const char* str) {
    const ImVec2 size = measure(body_, px(21), str, panel_width_);
    text(body_, px(21), ImVec2(origin_.x, origin_.y + cursor_), kDim, str, panel_width_);
    cursor_ += size.y + px(20);
}

bool Launcher::small_button(const char* label, ImVec2 at, const char* id, ImVec2* size_out) {
    const ImVec2 size = measure(body_bold_, px(18), label);
    const ImVec2 box(size.x + px(28), size.y + px(12));
    ImGui::SetCursorScreenPos(at);
    const bool pressed = ImGui::InvisibleButton(id, box);
    const float hot = anim(ImGui::GetItemID(), ImGui::IsItemHovered() || ImGui::IsItemFocused());
    list_->AddRectFilled(at, ImVec2(at.x + box.x, at.y + box.y), fade(kRed, hot * 0.25f));
    list_->AddRect(at, ImVec2(at.x + box.x, at.y + box.y), fade(mix(IM_COL32(255, 255, 255, 60), kRed, hot)), 0, 0, px(1.5f));
    text(body_bold_, px(18), ImVec2(at.x + px(14), at.y + px(6)), mix(kInk, IM_COL32(255, 120, 125, 255), hot), label);
    if (size_out) *size_out = box;
    return pressed;
}

bool Launcher::status(bool good, const char* heading, const char* detail, const char* button_label, const char* id) {
    const float height = px(64);
    const ImVec2 a(origin_.x, origin_.y + cursor_), b(origin_.x + panel_width_, a.y + height);
    const ImU32 accent = good ? kGreen : kAmber;
    list_->AddRectFilled(a, b, fade(IM_COL32(255, 255, 255, 10)));
    list_->AddRectFilled(a, ImVec2(a.x + px(4), b.y), fade(accent));
    list_->AddCircleFilled(ImVec2(a.x + px(26), a.y + height / 2), px(6), fade(accent));
    list_->AddCircleFilled(ImVec2(a.x + px(26), a.y + height / 2), px(12), fade(accent, 0.18f));
    text(body_bold_, px(21), ImVec2(a.x + px(46), a.y + px(9)), kInk, heading);
    text(body_, px(17), ImVec2(a.x + px(46), a.y + px(35)), kDim, detail);
    bool pressed = false;
    if (button_label) {
        const ImVec2 size = measure(body_bold_, px(18), button_label);
        pressed = small_button(button_label, ImVec2(b.x - size.x - px(28) - px(14), a.y + px(14)), id);
    }
    cursor_ += height + px(10);
    return pressed;
}

bool Launcher::big_button(const char* label, const char* id, bool pulse) {
    const ImVec2 size = measure(black_, px(40), label);
    const ImVec2 a(origin_.x, origin_.y + cursor_ + px(10)), b(a.x + size.x + px(110), a.y + px(64));
    ImGui::SetCursorScreenPos(a);
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(b.x - a.x + px(14), b.y - a.y));
    const float hot = anim(ImGui::GetItemID(), ImGui::IsItemHovered() || ImGui::IsItemFocused());
    if (pulse) {
        const float p = std::fmod(static_cast<float>(ImGui::GetTime()), 2.2f) / 2.2f / 0.7f;
        if (p < 1) {
            const float grow = px(18) * p;
            skewed_rect(list_, ImVec2(a.x - grow, a.y - grow), ImVec2(b.x + grow, b.y + grow), fade(kRed, 0.45f * (1 - p)),
                        fade(kRed, 0.45f * (1 - p)));
        }
    }
    skewed_rect(list_, a, b, mix(kRed, IM_COL32(255, 80, 88, 255), hot * 0.6f), mix(kRedDark, kRed, hot * 0.5f));
    text(black_, px(40), ImVec2(a.x + px(55) + px(7), a.y + px(10)), IM_COL32(255, 255, 255, 255), label);
    cursor_ += px(90);
    return pressed;
}

bool Launcher::tabs(const char* const* labels, int count, int* current) {
    float x = origin_.x;
    bool changed = false;
    for (int i = 0; i < count; ++i) {
        const ImVec2 size = measure(heading_, px(22), labels[i]);
        ImGui::SetCursorScreenPos(ImVec2(x, origin_.y + cursor_));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab", ImVec2(size.x, size.y + px(10)))) {
            *current = i;
            changed = true;
        }
        const float hot = anim(ImGui::GetItemID(), ImGui::IsItemHovered() || ImGui::IsItemFocused());
        ImGui::PopID();
        const bool on = *current == i;
        text(heading_, px(22), ImVec2(x, origin_.y + cursor_), on ? kInk : mix(kDim, kInk, hot), labels[i]);
        if (on) list_->AddRectFilled(ImVec2(x, origin_.y + cursor_ + size.y + px(4)), ImVec2(x + size.x, origin_.y + cursor_ + size.y + px(8)), fade(kRed));
        x += size.x + px(28);
    }
    cursor_ += px(48);
    return changed;
}

void Launcher::row(const char* heading, const char* detail) {
    text(body_bold_, px(20), ImVec2(origin_.x, origin_.y + cursor_ + px(4)), kInk, heading);
    if (detail) text(body_, px(16), ImVec2(origin_.x, origin_.y + cursor_ + px(29)), kDim, detail);
}

bool Launcher::segmented(const char* id, const char* const* labels, int count, int* current) {
    const float width = px(270), height = px(36);
    const ImVec2 a(origin_.x + panel_width_ - width, origin_.y + cursor_ + px(6));
    skewed_rect(list_, a, ImVec2(a.x + width, a.y + height), IM_COL32(255, 255, 255, 18), IM_COL32(255, 255, 255, 18));
    const float cell = width / static_cast<float>(count);
    bool changed = false;
    ImGui::PushID(id);
    for (int i = 0; i < count; ++i) {
        const ImVec2 c(a.x + cell * static_cast<float>(i), a.y);
        ImGui::SetCursorScreenPos(c);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##segment", ImVec2(cell, height)) && *current != i) {
            *current = i;
            changed = true;
        }
        const float hot = anim(ImGui::GetItemID(), ImGui::IsItemHovered() || ImGui::IsItemFocused());
        const float on = anim(ImGui::GetItemID() + 1, *current == i, 16);
        ImGui::PopID();
        if (on > 0.01f || hot > 0.01f) {
            skewed_rect(list_, c, ImVec2(c.x + cell, c.y + height), fade(mix(IM_COL32(255, 255, 255, 30), kRed, on), std::max(on, hot)),
                        fade(mix(IM_COL32(255, 255, 255, 30), kRedDark, on), std::max(on, hot)));
        }
        const ImVec2 size = measure(body_bold_, px(18), labels[i]);
        text(body_bold_, px(18), ImVec2(c.x + (cell - size.x) / 2 + px(3), c.y + (height - size.y) / 2), mix(kDim, IM_COL32(255, 255, 255, 255), std::max(on, hot)), labels[i]);
    }
    ImGui::PopID();
    cursor_ += px(58);
    list_->AddLine(ImVec2(origin_.x, origin_.y + cursor_ - px(6)), ImVec2(origin_.x + panel_width_, origin_.y + cursor_ - px(6)), fade(IM_COL32(255, 255, 255, 16)));
    return changed;
}

bool Launcher::slider(const char* id, float* value, float minimum, float maximum, const char* format) {
    const float width = px(270);
    ImGui::SetCursorScreenPos(ImVec2(origin_.x + panel_width_ - width, origin_.y + cursor_ + px(8)));
    ImGui::PushItemWidth(width);
    ImGui::PushFont(body_bold_, px(18));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(8), px(5)));
    const bool changed = ImGui::SliderFloat(id, value, minimum, maximum, format);
    ImGui::PopStyleVar();
    ImGui::PopFont();
    ImGui::PopItemWidth();
    cursor_ += px(52);
    list_->AddLine(ImVec2(origin_.x, origin_.y + cursor_ - px(6)), ImVec2(origin_.x + panel_width_, origin_.y + cursor_ - px(6)), fade(IM_COL32(255, 255, 255, 16)));
    return changed;
}

void Launcher::steps(int current, int total) {
    const float gap = px(10), width = (panel_width_ - gap * static_cast<float>(total - 1)) / static_cast<float>(total);
    for (int i = 0; i < total; ++i) {
        const ImVec2 a(origin_.x + (width + gap) * static_cast<float>(i), origin_.y + cursor_);
        const ImU32 colour = i <= current ? kRed : IM_COL32(255, 255, 255, 26);
        skewed_rect(list_, a, ImVec2(a.x + width, a.y + px(5)), colour, colour, 1.0f);
    }
    cursor_ += px(26);
}

// --- settings ---

struct Quality {
    float render_scale, anisotropy, effects, ambient_occlusion;
};
constexpr Quality kQualities[3] = {{1, 1, 0, 0}, {2, 8, 1, 0}, {2, 16, 1, 0.6f}};  // High: the defaults

int Launcher::quality_preset() const {
    for (int i = 0; i < 3; ++i) {
        const Quality& q = kQualities[i];
        if (setting("render_scale") == q.render_scale && setting("anisotropy") == q.anisotropy &&
            (setting("effects") != 0) == (q.effects != 0) && setting("ambient_occlusion") == q.ambient_occlusion) {
            return i;
        }
    }
    return -1;
}

void Launcher::apply_quality(int preset) {
    const Quality& q = kQualities[preset];
    set_setting("render_scale", q.render_scale);
    set_setting("anisotropy", q.anisotropy);
    set_setting("effects", q.effects);
    set_setting("ambient_occlusion", q.ambient_occlusion);
}

void Launcher::choose_disc() {
    const std::filesystem::path chosen = ask_for_disc_image(window_);
    if (chosen.empty()) return;
    std::string problem;
    if (std::optional<GameDisc> disc = check_game_disc(chosen, problem)) {
        const std::u8string path = chosen.u8string();
        setting_save_text("disc_image", std::string(path.begin(), path.end()));
        disc_ = std::move(disc);
        disc_problem_.clear();
        start_art_loader(chosen);
    } else {
        disc_problem_ = problem;
    }
}

// --- pages ---

void Launcher::page_play() {
    const Texts& t = T();
    title(t.ready);
    lead(t.ready_lead);
    if (disc_) {
        if (status(true, t.disc_ok, "Midnight Club 3: DUB Edition Remix \xC2\xB7 Xbox \xC2\xB7 PAL", t.change, "##disc")) choose_disc();
    } else if (status(false, t.disc_none, disc_problem_.empty() ? t.disc_none_sub : disc_problem_.c_str(), t.choose_iso, "##disc")) {
        choose_disc();
    }
    status(pad_connected_, pad_connected_ ? t.pad_on : t.pad_off, pad_connected_ ? t.pad_on_sub : t.pad_off_sub, nullptr, nullptr);
    char summary[160];
    const int preset = quality_preset();
    const char* quality = preset == 0 ? t.original : preset == 1 ? t.balanced : preset == 2 ? t.high : t.custom;
    std::snprintf(summary, sizeof(summary), "%s \xC2\xB7 %.0f fps \xC2\xB7 %s %s", setting("widescreen") != 0 ? "16:9" : "4:3",
                  setting("frame_rate") >= 60 ? 60.0f : 30.0f, t.quality, quality);
    if (status(true, summary, setting("effects") != 0 ? t.effects_sub : t.effects, t.edit, "##edit")) {
        page_ = Page::kSettings;
        page_since_ = static_cast<float>(ImGui::GetTime());
    }
    update_row(false);
    ImGui::SetCursorScreenPos(ImVec2(origin_.x, origin_.y + cursor_));
    if (big_button(t.play, "##play", disc_.has_value())) {
        if (disc_) {
            play_ = done_ = true;
        } else {
            choose_disc();
        }
    }
}

void Launcher::page_settings() {
    const Texts& t = T();
    title(t.settings);
    const char* tab_labels[] = {t.tab_game, t.tab_display, t.tab_graphics, t.tab_effects};
    tabs(tab_labels, 4, &settings_tab_);
    if (settings_tab_ == 0) {
        row(t.game_language, t.game_language_sub);
        static const int kLanguages[] = {0, 1, 4, 3, 5, 6};
        const char* names[] = {t.like_windows, "English", "Fran\xC3\xA7" "ais", "Deutsch", "Espa\xC3\xB1ol", "Italiano"};
        int index = 0;
        for (int i = 0; i < 6; ++i) {
            if (static_cast<int>(setting("game_language")) == kLanguages[i]) index = i;
        }
        ImGui::SetCursorScreenPos(ImVec2(origin_.x + panel_width_ - px(270), origin_.y + cursor_ + px(8)));
        ImGui::PushItemWidth(px(270));
        ImGui::PushFont(body_bold_, px(18));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(8), px(5)));
        if (ImGui::Combo("##language", &index, names, 6)) set_setting("game_language", static_cast<float>(kLanguages[index]));
        ImGui::PopStyleVar();
        ImGui::PopFont();
        ImGui::PopItemWidth();
        cursor_ += px(58);
        row(t.fps, t.fps_sub);
        const char* rates[] = {"30", "60"};
        int rate = setting("frame_rate") >= 60 ? 1 : 0;
        if (segmented("##fps", rates, 2, &rate)) set_setting("frame_rate", rate ? 60.0f : 30.0f);
        row(t.volume, nullptr);
        float volume = setting("volume");
        if (slider("##volume", &volume, 0, 100, "%.0f")) set_setting("volume", std::round(volume));
        row(t.music_volume, t.music_volume_sub);
        float music = setting("launcher_music");
        if (slider("##launcher_music", &music, 0, 100, "%.0f")) set_setting("launcher_music", std::round(music));
    } else if (settings_tab_ == 1) {
        row(t.aspect, t.aspect_sub);
        const char* aspects[] = {"4:3", "16:9"};
        int aspect = setting("widescreen") != 0 ? 1 : 0;
        if (segmented("##aspect", aspects, 2, &aspect)) set_setting("widescreen", static_cast<float>(aspect));
        row(t.window_mode, t.window_mode_sub);
        const char* modes[] = {t.windowed, t.fullscreen};
        int mode = setting("fullscreen") != 0 ? 1 : 0;
        if (segmented("##mode", modes, 2, &mode)) {
            set_setting("fullscreen", static_cast<float>(mode));
            nv2a_set_full_screen(mode != 0);
        }
        row(t.quality, t.quality_sub);
        const char* qualities[] = {t.original, t.balanced, t.high};
        int preset = quality_preset();
        if (segmented("##quality", qualities, 3, &preset)) apply_quality(preset);
        row(t.scale, t.scale_sub);
        const char* scales[] = {"1x", "2x", "3x", "4x"};
        int scale = std::clamp(static_cast<int>(setting("render_scale")) - 1, 0, 3);
        if (segmented("##scale", scales, 4, &scale)) set_setting("render_scale", static_cast<float>(scale + 1));
    } else if (settings_tab_ == 2) {
        row(t.anisotropy, nullptr);
        const char* levels[] = {"1x", "4x", "8x", "16x"};
        static const float kLevels[] = {1, 4, 8, 16};
        int level = 0;
        for (int i = 0; i < 4; ++i) {
            if (setting("anisotropy") >= kLevels[i]) level = i;
        }
        if (segmented("##anisotropy", levels, 4, &level)) set_setting("anisotropy", kLevels[level]);
        row(t.gpu_vertices, t.gpu_vertices_sub);
        const char* toggle[] = {t.off, t.on};
        int gpu = setting("gpu_vertices") != 0 ? 1 : 0;
        if (segmented("##gpu", toggle, 2, &gpu)) set_setting("gpu_vertices", static_cast<float>(gpu));
        row(t.effects, t.effects_sub);
        int effects = setting("effects") != 0 ? 1 : 0;
        if (segmented("##effects", toggle, 2, &effects)) set_setting("effects", static_cast<float>(effects));
    } else {
        struct Slider {
            const char* label;
            const char* key;
            float minimum, maximum;
        };
        const Slider sliders[] = {
            {t.sharpen, "sharpen", 0, 1},           {t.bloom, "bloom", 0, 1},
            {t.bloom_threshold, "bloom_threshold", 0, 1}, {t.saturation, "saturation", 0.5f, 1.5f},
            {t.contrast, "contrast", 0.8f, 1.3f},   {t.vignette, "vignette", 0, 0.5f},
            {t.occlusion, "ambient_occlusion", 0, 1}, {t.occlusion_radius, "ambient_occlusion_radius", 0.5f, 2},
        };
        for (const Slider& s : sliders) {
            row(s.label, nullptr);
            float value = setting(s.key);
            char id[48];
            std::snprintf(id, sizeof(id), "##%s", s.key);
            if (slider(id, &value, s.minimum, s.maximum, "%.2f")) set_setting(s.key, std::round(value * 100) / 100);
        }
    }
    cursor_ += px(8);
    if (small_button(t.defaults, ImVec2(origin_.x, origin_.y + cursor_), "##defaults")) {
        for (const char* key : {"game_language", "frame_rate", "volume", "launcher_music", "widescreen", "fullscreen", "render_scale",
                                "anisotropy", "gpu_vertices", "effects", "sharpen", "bloom", "bloom_threshold",
                                "saturation", "contrast", "vignette", "ambient_occlusion", "ambient_occlusion_radius"}) {
            set_setting(key, setting_default(key));
        }
        nv2a_set_full_screen(setting("fullscreen") != 0);
    }
    cursor_ += px(48);
}

void Launcher::page_controls() {
    const Texts& t = T();
    title(t.controls);
    lead(t.controls_lead);
    const int captured = g_captured_key.exchange(0);
    if (rebinding_ >= 0 && captured) {
        bind_key(static_cast<PadInput>(rebinding_), captured);
        rebinding_ = -1;
    }
    if (rebinding_ >= 0 && ImGui::GetTime() - rebinding_since_ > 6) {  // forgotten
        g_capturing = false;
        rebinding_ = -1;
    }
    const float row_height = px(33), column = panel_width_ / 2 + px(10);
    for (int i = 0; i < kPadInputs; ++i) {
        const KeyBinding& binding = key_binding(static_cast<PadInput>(i));
        const float x = origin_.x + (i % 2 ? column : 0.0f), y = origin_.y + cursor_ + row_height * static_cast<float>(i / 2);
        const float cell = panel_width_ / 2 - px(10);
        list_->AddRect(ImVec2(x, y + px(4)), ImVec2(x + px(70), y + row_height - px(4)), fade(kRed), 0, 0, px(1.5f));
        const ImVec2 pad_size = measure(body_bold_, px(15), binding.button);
        text(body_bold_, px(15), ImVec2(x + (px(70) - pad_size.x) / 2, y + (row_height - pad_size.y) / 2), IM_COL32(255, 110, 116, 255), binding.button);
        text(body_, px(17), ImVec2(x + px(80), y + px(7)), kInk, italian() ? binding.italian : binding.english);
        const bool waiting = rebinding_ == i;
        const std::string key = waiting ? t.press_key : key_name(bound_key(static_cast<PadInput>(i)));
        const ImVec2 key_size = measure(body_bold_, px(16), key.c_str());
        const ImVec2 box_a(x + cell - std::max(px(84), key_size.x + px(18)), y + px(3)), box_b(x + cell, y + row_height - px(3));
        ImGui::SetCursorScreenPos(box_a);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##key", ImVec2(box_b.x - box_a.x, box_b.y - box_a.y))) {
            rebinding_ = i;
            rebinding_since_ = static_cast<float>(ImGui::GetTime());
            g_captured_key = 0;
            g_capturing = true;
        }
        const float hot = anim(ImGui::GetItemID(), ImGui::IsItemHovered() || ImGui::IsItemFocused() || waiting);
        ImGui::PopID();
        list_->AddRectFilled(box_a, box_b, fade(mix(IM_COL32(255, 255, 255, 14), kRed, hot * (waiting ? 0.8f : 0.35f))));
        text(body_bold_, px(16), ImVec2(box_b.x - key_size.x - px(9), y + (row_height - key_size.y) / 2), kInk, key.c_str());
    }
    cursor_ += row_height * static_cast<float>((kPadInputs + 1) / 2) + px(12);
    if (small_button(t.reset_keys, ImVec2(origin_.x, origin_.y + cursor_), "##reset_keys")) reset_key_bindings();
    cursor_ += px(52);

    // The port's own keys, not rebindable.
    text(heading_, px(20), ImVec2(origin_.x, origin_.y + cursor_), kInk, t.system_keys);
    cursor_ += px(32);
    const char* enter = italian() ? "Invio" : "Enter";
    char alt_enter[32];
    std::snprintf(alt_enter, sizeof(alt_enter), "F11 \xC2\xB7 Alt+%s", enter);
    char navigation[48];
    std::snprintf(navigation, sizeof(navigation), "%s \xC2\xB7 %s \xC2\xB7 Esc", italian() ? "Frecce" : "Arrows", enter);
    const char* system[][2] = {{alt_enter, t.full_screen_key}, {"F10", t.effects_key}, {navigation, t.launcher_keys}};
    for (const auto& [keys, action] : system) {
        const ImVec2 size = measure(body_bold_, px(16), keys);
        const ImVec2 a(origin_.x, origin_.y + cursor_ + px(3));
        list_->AddRectFilled(a, ImVec2(a.x + size.x + px(18), a.y + row_height - px(6)), fade(IM_COL32(255, 255, 255, 14)));
        text(body_bold_, px(16), ImVec2(a.x + px(9), a.y + (row_height - px(6) - size.y) / 2), kInk, keys);
        text(body_, px(17), ImVec2(a.x + size.x + px(32), origin_.y + cursor_ + px(7)), kInk, action);
        cursor_ += row_height;
    }
    cursor_ += px(10);
    const ImVec2 note = measure(body_, px(16), t.pads_note, panel_width_);
    text(body_, px(16), ImVec2(origin_.x, origin_.y + cursor_), kDim, t.pads_note, panel_width_);
    cursor_ += note.y + px(8);
}

// The update notice: on the first page only when there is a newer version, on About always. An installed copy
// (the setup's uninstaller next to mc3.exe) updates itself; a portable one opens the release page.
void Launcher::update_row(bool always) {
    const Texts& t = T();
    int state, download;
    std::string version, url, setup_url;
    {
        std::lock_guard<std::mutex> lock(updates_.mutex);
        state = updates_.state;
        download = updates_.download;
        version = updates_.version;
        url = updates_.url;
        setup_url = updates_.setup_url;
    }
    if (setting("check_updates") == 0 || state == 3 || (!always && state != 2)) return;
    std::error_code ignored;
    const bool installed = !portable_mode() && std::filesystem::exists(program_directory() / "unins000.exe", ignored);
    const bool self_update = installed && !setup_url.empty() && download != 3;
    char found[96];
    std::snprintf(found, sizeof(found), t.update_found, version.c_str());
    if (download == 1) {
        status(false, t.downloading, found, nullptr, nullptr);
    } else if (state == 2) {
        if (status(false, download == 3 ? t.update_failed : found, self_update ? t.update_install_sub : t.update_sub,
                   self_update ? t.update_now : t.open, "##update")) {
            if (self_update) {
                std::lock_guard<std::mutex> lock(updates_.mutex);
                updates_.download = 1;
                if (download_thread_.joinable()) download_thread_.join();
                download_thread_ = std::thread(download_update, &updates_);
            } else {
                ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
    } else {
        status(true, state == 0 ? t.checking : t.up_to_date, "", nullptr, nullptr);
    }
}

void Launcher::page_about() {
    const Texts& t = T();
    title(t.about);
    lead(t.about_lead);
    update_row(true);
    char heading[64];
    std::snprintf(heading, sizeof(heading), "mc3port %s", MC3PORT_VERSION);
    if (status(true, heading, t.credits, "GitHub", "##github")) {
        ShellExecuteA(nullptr, "open", (std::string("https://github.com/") + kRepository).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (status(true, t.logs, t.logs_sub, t.open, "##logs")) {
        const std::wstring argument = L"/select,\"" + (data_directory() / "mc3port.log").wstring() + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", argument.c_str(), nullptr, SW_SHOWNORMAL);
    }
    cursor_ += px(8);
    const ImVec2 size = measure(body_, px(16), t.legal, panel_width_);
    text(body_, px(16), ImVec2(origin_.x, origin_.y + cursor_), kDim, t.legal, panel_width_);
    cursor_ += size.y + px(10);
}

void Launcher::page_wizard() {
    const Texts& t = T();
    steps(wizard_step_, 3);
    if (wizard_step_ == 0) {
        title(t.welcome);
        lead(t.welcome_lead);
        if (disc_) {
            if (status(true, t.disc_ok, "Midnight Club 3: DUB Edition Remix \xC2\xB7 Xbox \xC2\xB7 PAL", t.change, "##disc")) choose_disc();
        } else if (status(false, t.disc_none, disc_problem_.empty() ? t.disc_none_sub : disc_problem_.c_str(), t.choose_iso,
                          "##disc")) {
            choose_disc();
        }
        if (big_button(disc_ ? t.next : t.choose_iso, "##next", disc_.has_value())) {
            if (disc_) {
                wizard_step_ = 1;
                page_since_ = static_cast<float>(ImGui::GetTime());
            } else {
                choose_disc();
            }
        }
    } else if (wizard_step_ == 1) {
        title(t.quality_title);
        lead(t.quality_lead);
        row(t.quality, t.quality_sub);
        const char* qualities[] = {t.original, t.balanced, t.high};
        int preset = quality_preset();
        if (segmented("##quality", qualities, 3, &preset)) apply_quality(preset);
        row(t.aspect, t.aspect_sub);
        const char* aspects[] = {"4:3", "16:9"};
        int aspect = setting("widescreen") != 0 ? 1 : 0;
        if (segmented("##aspect", aspects, 2, &aspect)) set_setting("widescreen", static_cast<float>(aspect));
        row(t.fps, t.fps_sub);
        const char* rates[] = {"30", "60"};
        int rate = setting("frame_rate") >= 60 ? 1 : 0;
        if (segmented("##fps", rates, 2, &rate)) set_setting("frame_rate", rate ? 60.0f : 30.0f);
        if (big_button(t.next, "##next", false)) {
            wizard_step_ = 2;
            page_since_ = static_cast<float>(ImGui::GetTime());
        }
    } else {
        title(t.all_set);
        lead(t.all_set_lead);
        if (big_button(t.play, "##play", true)) play_ = done_ = true;
    }
}

// --- one frame ---

void Launcher::frame(float now, float dt) {
    const Texts& t = T();
    const ImVec2 screen(static_cast<float>(width_), static_cast<float>(height_));
    ImDrawList* back = ImGui::GetBackgroundDrawList();

    // Light trails and rain over the background.
    if (static_cast<float>(std::rand()) / RAND_MAX < dt * 5) {
        const float pick = static_cast<float>(std::rand()) / RAND_MAX;
        const ImU32 colour = pick < 0.55f ? IM_COL32(255, 64, 64, 255) : pick < 0.8f ? IM_COL32(255, 210, 122, 255) : IM_COL32(255, 255, 255, 255);
        trails_.push_back({-px(300), screen.y * (0.55f + 0.37f * static_cast<float>(std::rand()) / RAND_MAX),
                           px(1400 + 1600 * static_cast<float>(std::rand()) / RAND_MAX),
                           px(200 + 320 * static_cast<float>(std::rand()) / RAND_MAX), colour});
    }
    for (Trail& trail : trails_) {
        trail.x += trail.speed * dt;
        back->AddRectFilledMultiColor(ImVec2(trail.x - trail.length, trail.y), ImVec2(trail.x, trail.y + px(2.5f)),
                                      trail.colour & ~IM_COL32_A_MASK, trail.colour, trail.colour, trail.colour & ~IM_COL32_A_MASK);
    }
    trails_.erase(std::remove_if(trails_.begin(), trails_.end(), [&](const Trail& trail) { return trail.x - trail.length > screen.x; }),
                  trails_.end());
    if (drops_.size() != 140) {
        drops_.clear();
        for (int i = 0; i < 140; ++i) {
            drops_.push_back(ImVec4(screen.x * std::rand() / RAND_MAX, screen.y * std::rand() / RAND_MAX,
                                    10 + 12.0f * std::rand() / RAND_MAX, 0));
        }
    }
    for (ImVec4& drop : drops_) {
        const float speed = px(drop.z) * 60;
        drop.y += speed * dt;
        drop.x -= speed * 0.25f * dt;
        if (drop.y > screen.y) {
            drop.y = -px(20);
            drop.x = screen.x * 1.2f * std::rand() / RAND_MAX;
        }
        back->AddLine(ImVec2(drop.x, drop.y), ImVec2(drop.x - px(drop.z) * 0.25f, drop.y + px(drop.z)), IM_COL32(220, 220, 225, 34));
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(screen);
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    list_ = ImGui::GetWindowDrawList();
    const float since = now - start_time_;

    // Logo (from the disc) or the name.
    g_alpha = ease_out(since / 1.0f);
    const ImVec2 logo_at(px(86), px(54) - px(30) * (1 - g_alpha));
    if (logo_) {
        list_->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(logo_.Get())), logo_at, ImVec2(logo_at.x + px(460), logo_at.y + px(115)),
                        ImVec2(0, 0), ImVec2(1, 1), fade(IM_COL32_WHITE));
    } else {
        text(black_, px(64), logo_at, kInk, "MIDNIGHT CLUB 3");
        text(heading_, px(26), ImVec2(logo_at.x + px(4), logo_at.y + px(66)), kRed, "DUB EDITION REMIX");
    }
    std::string tag;
    for (const char* c = "MC3PORT \xC2\xB7 PC"; *c; ++c) tag += *c;
    g_alpha = ease_out((since - 0.2f) / 0.8f);
    float tag_x = px(92);
    for (size_t i = 0; i < tag.size(); ++i) {  // letter-spaced
        const size_t length = (static_cast<unsigned char>(tag[i]) >= 0xC0) ? 2 : 1;
        const std::string letter = tag.substr(i, length);
        text(italic_, px(16), ImVec2(tag_x, px(186)), kRed, letter.c_str());
        tag_x += measure(italic_, px(16), letter.c_str()).x + px(6);
        i += length - 1;
    }

    // Menu (hidden during the first-run guide).
    if (!wizard_) {
        const char* labels[] = {t.play, t.settings, t.controls, t.about, t.quit};
        for (int i = 0; i < 5; ++i) {
            g_alpha = ease_out((since - 0.25f - 0.07f * static_cast<float>(i)) / 0.7f);
            const float enter = (1 - g_alpha) * -px(60);
            const ImVec2 a(px(86) + enter, px(240) + px(64) * static_cast<float>(i));
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(i);
            const bool pressed = ImGui::InvisibleButton("##menu", ImVec2(px(360), px(54)));
            const bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused();
            if (static_cast<Page>(i) == page_ && since < 0.1f) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
            const Page target = static_cast<Page>(i);
            if ((pressed || ((hovered && ImGui::GetIO().MouseDelta.x * ImGui::GetIO().MouseDelta.x + ImGui::GetIO().MouseDelta.y * ImGui::GetIO().MouseDelta.y > 0) || focused)) &&
                target != Page::kQuit && page_ != target) {
                page_ = target;
                page_since_ = now;
            }
            if (pressed && target == Page::kQuit) done_ = true;
            if (pressed && target == Page::kPlay && disc_) play_ = done_ = true;
            const float on = anim(ImGui::GetID(labels[i]), page_ == target || (target == Page::kQuit && (hovered || focused)));
            const float shift = px(16) * on;
            const float bar = measure(black_, px(36), labels[i]).x + px(64);  // the label plus padding
            const ImVec2 b(a.x + bar * on + shift, a.y + px(54));
            if (on > 0.01f) {
                skewed_rect(list_, ImVec2(a.x + shift, a.y), b, kRed, kRedDark);
                skewed_rect(list_, ImVec2(a.x - px(14) + shift, a.y), ImVec2(a.x - px(8) + shift, b.y), fade(kChrome, on),
                            fade(kChrome, on));  // leaning like the red bar
            }
            text(black_, px(36), ImVec2(a.x + px(22) + shift, a.y + px(7)), mix(hovered || focused ? kInk : kDim, IM_COL32_WHITE, on), labels[i]);
        }
    }

    // The page panel, sliding in.
    const float enter = ease_out((now - page_since_) / 0.45f);
    g_alpha = enter * ease_out((since - 0.3f) / 0.6f);
    panel_width_ = px(560);
    const float panel_x = screen.x - px(96) - panel_width_ - px(56) + px(50) * (1 - enter);
    origin_ = ImVec2(panel_x + px(28), px(64) + px(26));
    cursor_ = 0;
    list_->ChannelsSplit(2);
    list_->ChannelsSetCurrent(1);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_alpha);
    if (wizard_) {
        page_wizard();
    } else {
        switch (page_) {
            case Page::kPlay: page_play(); break;
            case Page::kSettings: page_settings(); break;
            case Page::kControls: page_controls(); break;
            case Page::kAbout: page_about(); break;
            case Page::kQuit: break;
        }
    }
    ImGui::PopStyleVar();
    list_->ChannelsSetCurrent(0);
    const ImVec2 pa(panel_x, px(64)), pb(panel_x + panel_width_ + px(56), origin_.y + cursor_ + px(16));
    const float corner = px(24);
    const ImVec2 shape[] = {pa, ImVec2(pb.x, pa.y), ImVec2(pb.x, pb.y - corner), ImVec2(pb.x - corner, pb.y), ImVec2(pa.x, pb.y)};
    list_->AddConvexPolyFilled(shape, 5, fade(IM_COL32(12, 12, 14, 214)));
    list_->AddRectFilled(pa, ImVec2(pb.x, pa.y + px(3)), fade(kRed));
    list_->ChannelsMerge();

    // Bottom bar.
    g_alpha = ease_out((since - 0.4f) / 0.6f);
    const float bar_y = screen.y - px(56);
    list_->AddRectFilled(ImVec2(0, bar_y), screen, fade(IM_COL32(4, 4, 5, 196)));
    list_->AddLine(ImVec2(0, bar_y), ImVec2(screen.x, bar_y), fade(IM_COL32(255, 255, 255, 18)));
    float x = px(86);
    const float text_y = bar_y + px(17);
    const auto field = [&](const char* label, const char* value) {
        text(body_, px(18), ImVec2(x, text_y), kDim, label);
        x += measure(body_, px(18), label).x + px(6);
        text(body_bold_, px(18), ImVec2(x, text_y), kInk, value);
        x += measure(body_bold_, px(18), value).x + px(28);
    };
    field(t.bar_disc, disc_ ? t.bar_disc_ok : t.bar_disc_none);
    field(t.bar_pad, pad_connected_ ? t.bar_pad_on : t.bar_pad_off);
    field(t.bar_version, MC3PORT_VERSION);
    float right = screen.x - px(86);
    const auto hint = [&](const char* key, ImU32 colour, const char* label) {
        const float label_width = measure(body_, px(18), label).x;
        right -= label_width;
        text(body_, px(18), ImVec2(right, text_y), kDim, label);
        const float key_width = std::max(px(24), measure(body_bold_, px(15), key).x + px(12));
        right -= key_width + px(8);
        list_->AddRectFilled(ImVec2(right, text_y), ImVec2(right + key_width, text_y + px(22)), fade(colour), px(11));
        const ImVec2 key_size = measure(body_bold_, px(15), key);
        text(body_bold_, px(15), ImVec2(right + (key_width - key_size.x) / 2, text_y + (px(22) - key_size.y) / 2), IM_COL32(6, 6, 7, 255), key);
        right -= px(24);
    };
    hint(pad_connected_ ? "B" : "Esc", IM_COL32(255, 90, 90, 255), t.back);
    hint(pad_connected_ ? "A" : (italian() ? "Invio" : "Enter"), kGreen, t.select);
    g_alpha = 1;
    ImGui::End();

    // Enter or A with nothing focused (the mouse was used last): play from the first page.
    if (!wizard_ && page_ == Page::kPlay && disc_ && !ImGui::IsAnyItemFocused() &&
        (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown))) {
        play_ = done_ = true;
    }
    // MC3_LAUNCHER_AUTOPLAY=<seconds>: test scripts press Play.
    static const float autoplay = [] {
        char value[16] = {};
        return GetEnvironmentVariableA("MC3_LAUNCHER_AUTOPLAY", value, sizeof(value)) ? static_cast<float>(std::atof(value)) : -1.0f;
    }();
    if (autoplay >= 0 && since >= autoplay && disc_) play_ = done_ = true;

    // Back: to the first page, or a step back in the guide.
    if (rebinding_ < 0 && (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight))) {
        if (wizard_ && wizard_step_ > 0) {
            --wizard_step_;
            page_since_ = now;
        } else if (!wizard_ && page_ != Page::kPlay) {
            page_ = Page::kPlay;
            page_since_ = now;
        }
    }
}

std::optional<GameDisc> Launcher::run() {
    if (!start_graphics()) {
        log_line("launcher: Direct3D 11 is not available; asking for the disc with dialogs");
        stop_graphics();
        return open_game_disc(exe_directory_);
    }
    nv2a_set_window_hook(window_hook);
    disc_ = find_game_disc(exe_directory_, disc_problem_);
    wizard_ = !disc_;
    if (disc_) start_art_loader(disc_->path);
    if (setting("check_updates") != 0) update_thread_ = std::thread(check_for_updates, &updates_);
    char page[16] = {};  // MC3_LAUNCHER_PAGE=settings|controls|about: test screenshots
    if (GetEnvironmentVariableA("MC3_LAUNCHER_PAGE", page, sizeof(page)) && !wizard_) {
        page_ = !std::strcmp(page, "settings") ? Page::kSettings : !std::strcmp(page, "controls") ? Page::kControls
                : !std::strcmp(page, "about") ? Page::kAbout : Page::kPlay;
    }
    music_ = std::make_unique<LauncherMusic>();
    log_line("launcher: open (%s)", disc_ ? "disc found" : "first-run guide");

    LARGE_INTEGER frequency, start, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    float last = 0;
    while (!done_) {
        QueryPerformanceCounter(&now);
        const float time = static_cast<float>(now.QuadPart - start.QuadPart) / static_cast<float>(frequency.QuadPart);
        const float dt = std::min(time - last, 0.1f);
        last = time;
        if (IsIconic(window_)) {
            music_->update(dt, 0);
            Sleep(30);
            continue;
        }
        music_->update(dt, 1);
        if (time - pad_checked_ > 0.5f) {
            pad_checked_ = time;
            pad_connected_ = false;
            XINPUT_STATE state;
            for (DWORD i = 0; xinput_get_state_ && i < 4 && !pad_connected_; ++i) pad_connected_ = xinput_get_state_(i, &state) == ERROR_SUCCESS;
        }
        resize_if_needed();
        upload_art();
        {
            std::lock_guard<std::mutex> lock(updates_.mutex);
            if (updates_.download == 2) {  // verified: the installer replaces the files and reopens the game
                updates_.download = 0;
                ShellExecuteW(nullptr, L"open", updates_.setup_file.c_str(),
                              L"/SILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /UPDATE", nullptr, SW_SHOWNORMAL);
                log_line("launcher: installing the update, closing");
                done_ = true;
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_ui_mutex);
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            if (start_time_ == 0) start_time_ = page_since_ = time;
            frame(time, dt);
            ImGui::Render();
        }
        draw_background(time);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        swap_chain_->Present(1, 0);
    }
    log_line("launcher: %s", play_ ? "play" : "quit");
    stop_graphics();
    return play_ ? std::move(disc_) : std::nullopt;
}

}  // namespace

std::optional<GameDisc> run_launcher(HWND window, const std::filesystem::path& exe_directory) {
    Launcher launcher(window, exe_directory);
    return launcher.run();
}

}  // namespace mc3::pc
