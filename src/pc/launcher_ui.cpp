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
#include "pc/game_pause.h"
#include "pc/keyboard_bindings.h"
#include "pc/launcher_art.h"
#include "pc/launcher_music.h"
#include "pc/log.h"
#include "pc/nv2a.h"
#include "pc/paths.h"
#include "pc/renderer_d3d11.h"
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
    const char *resume, *back_to_game, *paused, *paused_lead, *exit_game, *exit_lead, *exit_now, *next_start, *menu_key;
    const char *fps_counter, *fps_counter_sub, *background_pause, *background_pause_sub, *resume_hint, *tabs_hint, *play_hint;
    const char *pad_keys, *pad_menu_keys, *motion_blur, *pages_hint, *move_hint, *dpad, *enter_hint;
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
    "Restore defaults", "Xbox controllers work as they are. Choose the keyboard keys here.", "Key? Esc cancels",
    "Restore keys", "PC KEYS", "Full screen on and off", "Modern effects on and off", "Menus: move, select, back",
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
    "RESUME", "BACK TO GAME", "PAUSED", "The game is paused. F1 or Esc takes you back.", "EXIT GAME",
    "Progress since the last save will be lost.", "EXIT", "Applies the next time the game starts", "In-game menu",
    "Frame counter", "In a corner while playing", "Pause in the background",
    "Opens the menu when you switch windows", "Resume", "Tabs", "Play",
    "D-pad \xC2\xB7 A \xC2\xB7 B \xC2\xB7 LB RB \xC2\xB7 Start", "Controller: move, select, back, tabs, play", "Motion blur", "Pages", "Move", "D-pad",
    "Move \xC2\xB7 right: into the page",
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
    "Tasto? Esc annulla", "Tasti predefiniti", "TASTI DEL PC", "Schermo intero s\xC3\xAC/no", "Effetti moderni s\xC3\xAC/no",
    "Menu: muoversi, scegliere, tornare",
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
    "RIPRENDI", "TORNA AL GIOCO", "IN PAUSA", "Il gioco \xC3\xA8 in pausa. F1 o Esc per tornare.", "ESCI DAL GIOCO",
    "I progressi dall'ultimo salvataggio andranno persi.", "ESCI", "Vale dal prossimo avvio del gioco", "Menu in gioco",
    "Contatore FPS", "In un angolo durante il gioco", "Pausa in secondo piano",
    "Apre il menu se cambi finestra", "Riprendi", "Schede", "Gioca",
    "Croce \xC2\xB7 A \xC2\xB7 B \xC2\xB7 LB RB \xC2\xB7 Start", "Controller: muoversi, scegliere, tornare, schede, gioca", "Sfocatura in velocit\xC3\xA0", "Pagine", "Muovi",
    "Croce", "Muovi \xC2\xB7 destra: nella pagina",
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

// The launcher's widgets are invisible buttons drawn by hand; ImGui leaves those out of keyboard and controller
// navigation unless asked.
constexpr ImGuiButtonFlags kNavigable = ImGuiButtonFlags_EnableNav;

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

// Any of keys pressed this frame (with key repeat).
bool key_pressed(std::initializer_list<ImGuiKey> keys) {
    for (const ImGuiKey key : keys) {
        if (ImGui::IsKeyPressed(key)) return true;
    }
    return false;
}

// Held keys repeat at a menu's pace (0.4 s, then every 0.15 s), not ImGui's 20 a second.
bool menu_key_pressed(std::initializer_list<ImGuiKey> keys) {
    for (const ImGuiKey key : keys) {
        if (ImGui::GetKeyPressedAmount(key, 0.4f, 0.15f) > 0) return true;
    }
    return false;
}

// --- shared state with the window thread ---

// ImGui's input queue is filled by the window thread. Recursive: ImGui's own calls (SetCapture on a click) send
// messages straight back into window_hook on the same thread.
std::recursive_mutex g_ui_mutex;
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
        g_captured_key = wparam == VK_ESCAPE ? -1 : key_from_message(wparam, lparam);  // Esc cancels
        g_capturing = false;
        *result = 0;
        return true;
    }
    std::lock_guard<std::recursive_mutex> lock(g_ui_mutex);
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) {
        *result = 1;
        return true;
    }
    return false;
}

std::string key_name(int virtual_key) {
    // Short names for the keys Windows spells out at length ("BARRA SPAZIATRICE", "Freccia SINISTRA").
    switch (virtual_key) {
        case VK_UP: return "\xE2\x86\x91";
        case VK_DOWN: return "\xE2\x86\x93";
        case VK_LEFT: return "\xE2\x86\x90";
        case VK_RIGHT: return "\xE2\x86\x92";
        case VK_SPACE: return italian() ? "Spazio" : "Space";
        case VK_RETURN: return italian() ? "Invio" : "Enter";
        case VK_BACK: return "Backspace";
        case VK_LSHIFT: case VK_RSHIFT: return "Shift";
        case VK_LCONTROL: case VK_RCONTROL: return "Ctrl";
        default: break;
    }
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
    Launcher(HWND window, const std::filesystem::path& exe_directory, bool in_game = false)
        : window_(window), exe_directory_(exe_directory), in_game_(in_game) {}
    std::optional<GameDisc> run();

    // In game: the same pages over the paused game, drawn by the renderer (overlay, on the GPU thread) into its back
    // buffer. Opening pauses the game; closing resumes it once the keys and buttons used are let go.
    void overlay(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* target, UINT width,
                 UINT height);
    void request_open() { open_requested_ = true; }
    void request_toggle() { toggle_requested_ = true; }
    bool visible() const { return visible_; }
    void set_disc_path(const std::filesystem::path& path) { disc_path_ = path; }

private:
    // In game, kPlay is the pause page and kQuit the page that exits the game.
    enum class Page { kPlay, kSettings, kControls, kAbout, kQuit };

    bool start_graphics();
    void start_imgui();
    void stop_graphics();
    void load_fonts();
    void frame_in_game(float now);
    void header(float since);
    void panel(float now, float since);
    void after_frame(float now, float since);
    void back_to_menu();
    void leave_page_on_left();
    // The page's first widget takes the keyboard and controller focus after Right or Enter in the menu.
    void claim_focus() {
        if (!focus_panel_) return;
        ImGui::SetKeyboardFocusHere();
        focus_panel_ = false;
    }
    // Keyboard and controller: the menu on the left (handled by side_menu) or the page (ImGui's navigation).
    void configure_navigation() {
        constexpr ImGuiConfigFlags kNavigation = ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        ImGuiIO& io = ImGui::GetIO();
        if (menu_has_focus_ && !wizard_) {
            io.ConfigFlags &= ~kNavigation;
        } else {
            io.ConfigFlags |= kNavigation;
        }
    }
    void draw_frame_counter(float now);
    void draw_menu_hint(float now);
    void side_menu(float now, float since, const char* const* labels, const Page* targets, int count);
    void bottom_bar(float since);
    void resume();
    void stop_rumble();
    void poll_gamepads(float now);  // every XInput port into ImGui's keys, the left stick as a d-pad
    void focus_ring();              // the launcher's focus look on a standard ImGui widget
    float pad_icon(ImDrawList* list, const char* name, ImVec2 at, float h, bool draw = true);
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
    bool status(bool good, const char* heading, const char* detail, const char* button_label, const char* id,
                bool pulse = false);
    bool small_button(const char* label, ImVec2 at, const char* id, ImVec2* size_out = nullptr);
    bool big_button(const char* label, const char* id, bool pulse);
    bool tabs(const char* const* labels, int count, int* current);
    // `restart`: a key the game reads once; in game, the detail says it applies at the next start.
    void row(const char* heading, const char* detail, bool restart = false);
    bool segmented(const char* id, const char* const* labels, int count, int* current);
    bool slider(const char* id, float* value, float minimum, float maximum, const char* format);
    void steps(int current, int total);

    void page_play();
    void page_settings();
    void page_controls();
    void page_about();
    void update_row(bool always);
    bool update_available();
    void page_wizard();
    void page_paused();
    void page_exit();
    // The file dialog makes the game window its owner, which needs the window thread: never while g_ui_mutex is
    // held (that thread waits for it in window_hook). Clicks only ask for it; run() opens it between frames.
    void choose_disc() { choose_disc_requested_ = true; }
    void run_disc_dialog();
    void set_setting(const char* key, float value) { setting_save(key, value); }
    int quality_preset() const;
    void apply_quality(int preset);

    HWND window_;
    std::filesystem::path exe_directory_;
    const bool in_game_;
    std::filesystem::path disc_path_;  // in game: for the logo
    std::atomic<bool> open_requested_{false}, toggle_requested_{false}, visible_{false};
    bool resuming_ = false;
    bool menu_has_focus_ = true, focus_panel_ = false, left_pending_ = false;
    int menu_cursor_ = 0;
    ImGuiID left_from_ = 0;
    float resume_since_ = 0, now_ = 0;
    LARGE_INTEGER clock_start_{};
    uint32_t counted_frames_ = 0;
    float counted_since_ = -1, frames_per_second_ = 0;
    decltype(&XInputSetState) xinput_set_state_ = nullptr;
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
    bool pad_present_[4] = {};
    float pad_rescan_ = -10;
    int stick_x_ = 0, stick_y_ = 0;  // the left stick as a d-pad direction, with hysteresis
    float stick_repeat_at_ = 0;      // when the held stick next counts as a press (kStickDelay, then kStickRate)

    Page page_ = Page::kPlay;
    float page_since_ = 0;
    int settings_tab_ = 0;
    float tab_since_ = 0;                 // the settings tab's content fades in from here
    int hover_item_ = -1;                 // side menu: the item the mouse rests on, selected after a short dwell
    float hover_since_ = 0;
    bool hover_armed_ = false;
    int cancel_frame_ = -1;               // the frame a rebinding was cancelled on: B does nothing else then
    bool wizard_ = false;
    int wizard_step_ = 0;
    int rebinding_ = -1;
    float rebinding_since_ = 0;
    bool done_ = false, play_ = false, choose_disc_requested_ = false;
    float start_time_ = 0;

    ImDrawList* list_ = nullptr;
    ImVec2 origin_;  // panel content top left
    float panel_width_ = 0, cursor_ = 0, row_extra_ = 0;
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
    start_imgui();
    return true;
}

// ImGui on device_ and context_, with the launcher's fonts and colours, and XInput for the controller status.
void Launcher::start_imgui() {
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
    colours[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);  // the widgets draw their own focus (anim, focus_ring)
    colours[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.1f);

    xinput_ = LoadLibraryW(L"xinput1_4.dll");
    if (!xinput_) xinput_ = LoadLibraryW(L"xinput9_1_0.dll");
    if (xinput_) {
        xinput_get_state_ = reinterpret_cast<decltype(&XInputGetState)>(GetProcAddress(xinput_, "XInputGetState"));
        xinput_set_state_ = reinterpret_cast<decltype(&XInputSetState)>(GetProcAddress(xinput_, "XInputSetState"));
    }
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
        std::lock_guard<std::recursive_mutex> lock(g_ui_mutex);
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
    if (in_game_) return;  // the game is the background
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
    claim_focus();
    const bool pressed = ImGui::InvisibleButton(id, box, kNavigable);
    const float hot = anim(ImGui::GetItemID(), ImGui::IsItemHovered() || ImGui::IsItemFocused());
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    list_->AddRectFilled(at, ImVec2(at.x + box.x, at.y + box.y), fade(kRed, hot * 0.25f));
    list_->AddRect(at, ImVec2(at.x + box.x, at.y + box.y), fade(mix(IM_COL32(255, 255, 255, 60), kRed, hot)), 0, 0, px(1.5f));
    text(body_bold_, px(18), ImVec2(at.x + px(14), at.y + px(6)), mix(kInk, IM_COL32(255, 120, 125, 255), hot), label);
    if (size_out) *size_out = box;
    return pressed;
}

bool Launcher::status(bool good, const char* heading, const char* detail, const char* button_label, const char* id,
                      bool pulse) {
    const float height = px(64);
    const ImVec2 a(origin_.x, origin_.y + cursor_), b(origin_.x + panel_width_, a.y + height);
    const ImU32 accent = good ? kGreen : kAmber;
    list_->AddRectFilled(a, b, fade(pulse ? IM_COL32(255, 178, 30, 22) : IM_COL32(255, 255, 255, 10)));
    list_->AddRectFilled(a, ImVec2(a.x + px(4), b.y), fade(accent));
    const ImVec2 dot(a.x + px(26), a.y + height / 2);
    list_->AddCircleFilled(dot, px(6), fade(accent));
    list_->AddCircleFilled(dot, px(12), fade(accent, 0.18f));
    if (pulse) {
        const float p = std::fmod(static_cast<float>(ImGui::GetTime()), 1.8f) / 1.8f;
        list_->AddCircle(dot, px(12) + px(14) * p, fade(accent, 0.6f * (1 - p)), 0, px(2));
    }
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
    claim_focus();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(b.x - a.x + px(14), b.y - a.y), kNavigable);
    const float hot = anim(ImGui::GetItemID(), ImGui::IsItemHovered() || ImGui::IsItemFocused());
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
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
        if (i == 0) claim_focus();
        if (ImGui::InvisibleButton("##tab", ImVec2(size.x, size.y + px(10)), kNavigable)) {
            *current = i;
            changed = true;
        }
        const float hot = anim(ImGui::GetItemID(), ImGui::IsItemHovered() || ImGui::IsItemFocused());
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::PopID();
        const bool on = *current == i;
        text(heading_, px(22), ImVec2(x, origin_.y + cursor_), on ? kInk : mix(kDim, kInk, hot), labels[i]);
        if (on) list_->AddRectFilled(ImVec2(x, origin_.y + cursor_ + size.y + px(4)), ImVec2(x + size.x, origin_.y + cursor_ + size.y + px(8)), fade(kRed));
        x += size.x + px(28);
    }
    cursor_ += px(48);
    return changed;
}

void Launcher::row(const char* heading, const char* detail, bool restart) {
    text(body_bold_, px(20), ImVec2(origin_.x, origin_.y + cursor_ + px(4)), kInk, heading);
    const bool next_start = restart && in_game_;
    if (next_start) detail = T().next_start;
    if (!detail) return;
    // Left of the control (270 wide): a longer detail wraps, and the row grows by the extra lines.
    const float wrap = panel_width_ - px(290);
    text(body_, px(16), ImVec2(origin_.x, origin_.y + cursor_ + px(29)), next_start ? kAmber : kDim, detail, wrap);
    row_extra_ = std::max(0.0f, measure(body_, px(16), detail, wrap).y - px(20));
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
        if (i == *current) claim_focus();
        if (ImGui::InvisibleButton("##segment", ImVec2(cell, height), kNavigable) && *current != i) {
            *current = i;
            changed = true;
        }
        const float hot = anim(ImGui::GetItemID(), ImGui::IsItemHovered() || ImGui::IsItemFocused());
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
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
    cursor_ += px(58) + row_extra_;
    row_extra_ = 0;
    list_->AddLine(ImVec2(origin_.x, origin_.y + cursor_ - px(6)), ImVec2(origin_.x + panel_width_, origin_.y + cursor_ - px(6)), fade(IM_COL32(255, 255, 255, 16)));
    return changed;
}

bool Launcher::slider(const char* id, float* value, float minimum, float maximum, const char* format) {
    const float width = px(270);
    ImGui::SetCursorScreenPos(ImVec2(origin_.x + panel_width_ - width, origin_.y + cursor_ + px(8)));
    ImGui::PushItemWidth(width);
    ImGui::PushFont(body_bold_, px(18));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(8), px(5)));
    claim_focus();
    const bool changed = ImGui::SliderFloat(id, value, minimum, maximum, format);
    focus_ring();
    ImGui::PopStyleVar();
    ImGui::PopFont();
    ImGui::PopItemWidth();
    cursor_ += px(52) + row_extra_;
    row_extra_ = 0;
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

void Launcher::run_disc_dialog() {
    log_line("launcher: opening the file dialog");
    std::filesystem::path chosen;
    try {
        chosen = ask_for_disc_image(window_);
    } catch (const std::exception& e) {
        log_line("launcher: file dialog failed: %s (%s)", e.what(), typeid(e).name());
        return;
    }
    log_line("launcher: file dialog closed (%s)", chosen.empty() ? "nothing chosen" : "file chosen");
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
    update_row(false);
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
    const float now = static_cast<float>(ImGui::GetTime());
    const int tab_before = settings_tab_;
    title(t.settings);
    const char* tab_labels[] = {t.tab_game, t.tab_display, t.tab_graphics, t.tab_effects};
    // LB and RB (L1 and R1), Q and E: the previous and next tab.
    if (rebinding_ < 0 && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        const int step = key_pressed({ImGuiKey_GamepadL1, ImGuiKey_Q, ImGuiKey_PageUp})     ? 3
                         : key_pressed({ImGuiKey_GamepadR1, ImGuiKey_E, ImGuiKey_PageDown}) ? 1
                                                                                            : 0;
        if (step) {
            settings_tab_ = (settings_tab_ + step) % 4;
            if (!menu_has_focus_) focus_panel_ = true;
        }
    }
    tabs(tab_labels, 4, &settings_tab_);
    if (settings_tab_ != tab_before) tab_since_ = now;
    // The tab's content comes in like a page: fading, from a little to the right.
    const float tab_enter = ease_out((now - tab_since_) / 0.3f);
    const float panel_alpha = g_alpha, origin_x = origin_.x;
    g_alpha *= tab_enter;
    origin_.x += px(24) * (1 - tab_enter);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_alpha);
    if (settings_tab_ == 0) {
        row(t.game_language, t.game_language_sub, true);
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
        claim_focus();
        if (ImGui::Combo("##language", &index, names, 6)) set_setting("game_language", static_cast<float>(kLanguages[index]));
        focus_ring();
        ImGui::PopStyleVar();
        ImGui::PopFont();
        ImGui::PopItemWidth();
        cursor_ += px(58) + row_extra_;
        row_extra_ = 0;
        row(t.fps, t.fps_sub, true);
        const char* rates[] = {"30", "60"};
        int rate = setting("frame_rate") >= 60 ? 1 : 0;
        if (segmented("##fps", rates, 2, &rate)) set_setting("frame_rate", rate ? 60.0f : 30.0f);
        row(t.volume, nullptr);
        float volume = setting("volume");
        if (slider("##volume", &volume, 0, 100, "%.0f")) set_setting("volume", std::round(volume));
        if (!in_game_) {  // no launcher music in the game
            row(t.music_volume, t.music_volume_sub);
            float music = setting("launcher_music");
            if (slider("##launcher_music", &music, 0, 100, "%.0f")) set_setting("launcher_music", std::round(music));
        }
        row(t.background_pause, t.background_pause_sub);
        const char* toggle[] = {t.off, t.on};
        int pause = setting("pause_in_background") != 0 ? 1 : 0;
        if (segmented("##background_pause", toggle, 2, &pause)) set_setting("pause_in_background", static_cast<float>(pause));
    } else if (settings_tab_ == 1) {
        row(t.aspect, t.aspect_sub, true);
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
        row(t.quality, t.quality_sub, true);
        const char* qualities[] = {t.original, t.balanced, t.high};
        int preset = quality_preset();
        if (segmented("##quality", qualities, 3, &preset)) apply_quality(preset);
        row(t.scale, t.scale_sub, true);
        const char* scales[] = {"1x", "2x", "3x", "4x"};
        int scale = std::clamp(static_cast<int>(setting("render_scale")) - 1, 0, 3);
        if (segmented("##scale", scales, 4, &scale)) set_setting("render_scale", static_cast<float>(scale + 1));
        row(t.fps_counter, t.fps_counter_sub);
        const char* toggle[] = {t.off, t.on};
        int counter = setting("fps_counter") != 0 ? 1 : 0;
        if (segmented("##fps_counter", toggle, 2, &counter)) set_setting("fps_counter", static_cast<float>(counter));
    } else if (settings_tab_ == 2) {
        row(t.anisotropy, nullptr, true);
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
        if (segmented("##effects", toggle, 2, &effects)) {
            set_setting("effects", static_cast<float>(effects));
            renderer_set_effects(effects != 0);
        }
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
            {t.motion_blur, "motion_blur", 0, 1},
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
        for (const char* key : {"game_language", "frame_rate", "volume", "launcher_music", "pause_in_background", "widescreen",
                                "fullscreen", "fps_counter", "render_scale",
                                "anisotropy", "gpu_vertices", "effects", "sharpen", "bloom", "bloom_threshold",
                                "saturation", "contrast", "vignette", "motion_blur", "ambient_occlusion", "ambient_occlusion_radius"}) {
            set_setting(key, setting_default(key));
        }
        nv2a_set_full_screen(setting("fullscreen") != 0);
        renderer_set_effects(setting("effects") != 0);
    }
    cursor_ += px(48);
    ImGui::PopStyleVar();
    g_alpha = panel_alpha;
    origin_.x = origin_x;
}

void Launcher::page_controls() {
    const Texts& t = T();
    title(t.controls);
    lead(t.controls_lead);
    const int captured = g_captured_key.exchange(0);
    if (rebinding_ >= 0 && captured) {
        if (captured > 0) bind_key(static_cast<PadInput>(rebinding_), captured);  // -1: Esc, cancelled
        rebinding_ = -1;
    }
    if (rebinding_ >= 0 && (ImGui::IsMouseClicked(0) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight))) {  // a click elsewhere or B
        g_capturing = false;
        rebinding_ = -1;
        cancel_frame_ = ImGui::GetFrameCount();
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
        const float icon_h = px(22), icon_w = pad_icon(list_, binding.button, ImVec2(0, 0), icon_h, false);
        pad_icon(list_, binding.button, ImVec2(x + (px(70) - icon_w) / 2, y + (row_height - icon_h) / 2), icon_h);
        const bool waiting = rebinding_ == i;
        const std::string key = waiting ? t.press_key : key_name(bound_key(static_cast<PadInput>(i)));
        const ImVec2 key_size = measure(body_bold_, px(16), key.c_str());
        const ImVec2 box_a(x + cell - std::max(px(60), key_size.x + px(18)), y + px(3)), box_b(x + cell, y + row_height - px(3));
        list_->PushClipRect(ImVec2(x + px(80), y), ImVec2(box_a.x - px(6), y + row_height), true);  // never under the key box
        text(body_, px(15), ImVec2(x + px(80), y + px(8)), kInk, italian() ? binding.italian : binding.english);
        list_->PopClipRect();
        ImGui::SetCursorScreenPos(box_a);
        ImGui::PushID(i);
        claim_focus();
        if (ImGui::InvisibleButton("##key", ImVec2(box_b.x - box_a.x, box_b.y - box_a.y), kNavigable)) {
            rebinding_ = i;
            rebinding_since_ = static_cast<float>(ImGui::GetTime());
            g_captured_key = 0;
            g_capturing = true;
        }
        const float hot = anim(ImGui::GetItemID(), ImGui::IsItemHovered() || ImGui::IsItemFocused() || waiting);
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
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
    std::snprintf(navigation, sizeof(navigation), "%s \xC2\xB7 %s \xC2\xB7 Esc \xC2\xB7 Q E", italian() ? "Frecce" : "Arrows", enter);
    const char* system[][2] = {{"F1 \xC2\xB7 Back+Start", t.menu_key}, {alt_enter, t.full_screen_key}, {"F10", t.effects_key},
                               {navigation, t.launcher_keys}, {t.pad_keys, t.pad_menu_keys}};
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
                   self_update ? t.update_now : t.open, "##update", download != 3)) {
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

// A newer release is known and not being installed: the first page and the guide show the notice at the top, the
// side menu marks About.
bool Launcher::update_available() {
    if (setting("check_updates") == 0) return false;
    std::lock_guard<std::mutex> lock(updates_.mutex);
    return updates_.state == 2 && updates_.download != 2;
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
        update_row(false);
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
        row(t.fps, t.fps_sub, true);
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

    header(since);

    // Menu (hidden during the first-run guide).
    if (!wizard_) {
        const char* labels[] = {t.play, t.settings, t.controls, t.about, t.quit};
        const Page targets[] = {Page::kPlay, Page::kSettings, Page::kControls, Page::kAbout, Page::kQuit};
        side_menu(now, since, labels, targets, 5);
    }
    panel(now, since);
    bottom_bar(since);
    g_alpha = 1;
    ImGui::End();
    after_frame(now, since);
}

// The logo (from the disc) or the name, and the port's tag under it.
void Launcher::header(float since) {
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
}

// The current page in the panel on the right, sliding in.
void Launcher::panel(float now, float since) {
    const float enter = ease_out((now - page_since_) / 0.45f);
    g_alpha = enter * ease_out((since - 0.3f) / 0.6f);
    panel_width_ = px(560);
    const float panel_x = static_cast<float>(width_) - px(96) - panel_width_ - px(56) + px(50) * (1 - enter);
    origin_ = ImVec2(panel_x + px(28), px(64) + px(26));
    cursor_ = 0;
    list_->ChannelsSplit(2);
    list_->ChannelsSetCurrent(1);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_alpha);
    if (wizard_) {
        page_wizard();
    } else {
        switch (page_) {
            case Page::kPlay: in_game_ ? page_paused() : page_play(); break;
            case Page::kSettings: page_settings(); break;
            case Page::kControls: page_controls(); break;
            case Page::kAbout: page_about(); break;
            case Page::kQuit:
                if (in_game_) page_exit();
                break;
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
}

// Keys acting on the whole launcher, after the frame's widgets.
void Launcher::after_frame(float now, float since) {
    // MC3_LAUNCHER_AUTOPLAY=<seconds>: test scripts press Play.
    static const float autoplay = [] {
        char value[16] = {};
        return GetEnvironmentVariableA("MC3_LAUNCHER_AUTOPLAY", value, sizeof(value)) ? static_cast<float>(std::atof(value)) : -1.0f;
    }();
    if (autoplay >= 0 && since >= autoplay && disc_) play_ = done_ = true;
    // Start on a controller plays from any page.
    if (!wizard_ && disc_ && rebinding_ < 0 && ImGui::IsKeyPressed(ImGuiKey_GamepadStart)) play_ = done_ = true;

    // Back: from the page to the menu, then to the first page; a step back in the guide.
    if (rebinding_ < 0 && cancel_frame_ != ImGui::GetFrameCount() &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight))) {
        if (wizard_) {
            if (wizard_step_ > 0) {
                --wizard_step_;
                page_since_ = now;
            }
        } else if (!menu_has_focus_) {
            back_to_menu();
        } else if (page_ != Page::kPlay) {
            page_ = Page::kPlay;
            menu_cursor_ = 0;
            page_since_ = now;
        }
    }
    leave_page_on_left();
}

// The page's widgets go back to ImGui's own navigation off, the menu on the left takes the keys again.
void Launcher::back_to_menu() {
    menu_has_focus_ = true;
    focus_panel_ = false;
    ImGui::SetNavID(0, ImGuiNavLayer_Main, 0, ImRect());
}

// Left on the page's leftmost widget (ImGui found nothing further left): back to the menu.
void Launcher::leave_page_on_left() {
    const ImGuiID nav_id = ImGui::GetCurrentContext()->NavId;
    if (left_pending_ && nav_id == left_from_) back_to_menu();
    left_pending_ = false;
    if (!menu_has_focus_ && !wizard_ && rebinding_ < 0 && !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        key_pressed({ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft})) {
        left_pending_ = true;
        left_from_ = nav_id;
    }
}

// The menu on the left: one item per page, the current one on a leaning red bar. Moving the mouse over an item opens
// its page; Quit (launcher) and Resume (game) act when pressed. Keyboard and controller: up and down move through the
// items (and their pages), right enters the page, Enter or A acts or enters.
void Launcher::side_menu(float now, float since, const char* const* labels, const Page* targets, int count) {
    const auto action_only = [&](int i) { return targets[i] == Page::kQuit && !in_game_; };
    const auto select = [&](int i) {
        menu_cursor_ = i;
        if (!action_only(i) && page_ != targets[i]) {
            page_ = targets[i];
            page_since_ = now;
        }
    };
    const auto act = [&](int i) {
        if (in_game_ && targets[i] == Page::kPlay) {
            resume();
        } else if (!in_game_ && targets[i] == Page::kQuit) {
            done_ = true;
        } else if (!in_game_ && targets[i] == Page::kPlay && disc_) {
            play_ = done_ = true;
        } else {
            menu_has_focus_ = false;  // into the page
            focus_panel_ = true;
        }
    };
    if (menu_cursor_ >= count || (!action_only(menu_cursor_) && targets[menu_cursor_] != page_)) {
        for (int i = 0; i < count; ++i) {
            if (targets[i] == page_) menu_cursor_ = i;
        }
    }
    if (menu_has_focus_ && rebinding_ < 0) {
        if (menu_key_pressed({ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp})) {
            select((menu_cursor_ + count - 1) % count);
        } else if (menu_key_pressed({ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown})) {
            select((menu_cursor_ + 1) % count);
        } else if (key_pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_GamepadFaceDown})) {
            act(menu_cursor_);
        } else if (key_pressed({ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight}) && !action_only(menu_cursor_)) {
            menu_has_focus_ = false;
            focus_panel_ = true;
        }
    }
    // LB and RB (L1 and R1), Q and E: the previous and next page, from anywhere; on Settings they change its tab.
    if (rebinding_ < 0 && page_ != Page::kSettings && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        const bool previous = key_pressed({ImGuiKey_GamepadL1, ImGuiKey_Q, ImGuiKey_PageUp});
        const bool next = key_pressed({ImGuiKey_GamepadR1, ImGuiKey_E, ImGuiKey_PageDown});
        if (previous || next) {
            if (!menu_has_focus_) back_to_menu();
            int i = menu_cursor_;
            do {
                i = (i + (next ? 1 : count - 1)) % count;
            } while (action_only(i));
            select(i);
        }
    }
    for (int i = 0; i < count; ++i) {
        g_alpha = ease_out((since - 0.25f - 0.07f * static_cast<float>(i)) / 0.7f);
        const float enter = (1 - g_alpha) * -px(60);
        const ImVec2 a(px(86) + enter, px(240) + px(64) * static_cast<float>(i));
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        const bool pressed = ImGui::InvisibleButton("##menu", ImVec2(px(360), px(54)));  // the keys are handled above
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const ImVec2 mouse = ImGui::GetIO().MouseDelta;
        if (hovered && mouse.x * mouse.x + mouse.y * mouse.y > 0 && hover_item_ != i) {  // the mouse arrived here
            hover_item_ = i;
            hover_since_ = now;
            hover_armed_ = true;
        }
        if (hovered && hover_armed_ && hover_item_ == i && now - hover_since_ > 0.12f) {  // and rested a moment
            hover_armed_ = false;
            select(i);
        }
        if (!hovered && hover_item_ == i) hover_item_ = -1;
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (pressed) {
            select(i);
            act(i);
        }
        // The current page's item, or Quit while the cursor is on it.
        const bool current = action_only(menu_cursor_) ? menu_cursor_ == i : targets[i] == page_;
        const float on = anim(ImGui::GetID(labels[i]), current);
        const float shift = px(16) * on;
        const float bar = measure(black_, px(36), labels[i]).x + px(64);  // the label plus padding
        const ImVec2 b(a.x + bar * on + shift, a.y + px(54));
        if (on > 0.01f) {
            // Brighter while the keys move through the menu; darker once they work on the page.
            const float lit = anim(ImGui::GetID(labels[i]) + 1, menu_has_focus_ || wizard_);
            skewed_rect(list_, ImVec2(a.x + shift, a.y), b, mix(kRedDark, kRed, 0.5f + 0.5f * lit), kRedDark);
            skewed_rect(list_, ImVec2(a.x - px(14) + shift, a.y), ImVec2(a.x - px(8) + shift, b.y),
                        fade(kChrome, on * (0.4f + 0.6f * lit)), fade(kChrome, on * (0.4f + 0.6f * lit)));
        }
        text(black_, px(36), ImVec2(a.x + px(22) + shift, a.y + px(7)), mix(hovered ? kInk : kDim, IM_COL32_WHITE, on), labels[i]);
        if (targets[i] == Page::kAbout && update_available()) {  // a newer version: a dot after the label
            const ImVec2 dot(a.x + px(22) + shift + measure(black_, px(36), labels[i]).x + px(16), a.y + px(16));
            const float p = std::fmod(static_cast<float>(ImGui::GetTime()), 1.8f) / 1.8f;
            list_->AddCircleFilled(dot, px(6), fade(kAmber));
            list_->AddCircle(dot, px(6) + px(10) * p, fade(kAmber, 0.6f * (1 - p)), 0, px(2));
        }
    }
}

// Disc, controller and version on the left; the keys that select and go back on the right.
void Launcher::bottom_bar(float since) {
    const Texts& t = T();
    const ImVec2 screen(static_cast<float>(width_), static_cast<float>(height_));
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
    if (!in_game_) field(t.bar_disc, disc_ ? t.bar_disc_ok : t.bar_disc_none);
    field(t.bar_pad, pad_connected_ ? t.bar_pad_on : t.bar_pad_off);
    field(t.bar_version, MC3PORT_VERSION);
    float right = screen.x - px(86);
    const bool pad = pad_connected_;
    const auto hint = [&](const char* key, ImU32 colour, const char* label) {
        const float label_width = measure(body_, px(18), label).x;
        right -= label_width;
        text(body_, px(18), ImVec2(right, text_y), kDim, label);
        if (pad) {  // the button's glyph
            const float key_width = pad_icon(list_, key, ImVec2(0, 0), px(22), false);
            right -= key_width + px(8);
            pad_icon(list_, key, ImVec2(right, text_y), px(22));
        } else {  // the key's name in a pill
            const float key_width = std::max(px(24), measure(body_bold_, px(15), key).x + px(12));
            right -= key_width + px(8);
            list_->AddRectFilled(ImVec2(right, text_y), ImVec2(right + key_width, text_y + px(22)), fade(colour), px(11));
            const ImVec2 key_size = measure(body_bold_, px(15), key);
            text(body_bold_, px(15), ImVec2(right + (key_width - key_size.x) / 2, text_y + (px(22) - key_size.y) / 2), IM_COL32(6, 6, 7, 255), key);
        }
        right -= px(24);
    };
    // Every key of this screen, read left to right: move, select, back, tabs or pages, play or resume (drawn from the
    // right). The controller's buttons when one is connected, else the keyboard's.
    if (in_game_) {
        hint(pad ? "Start" : "F1", kChrome, t.resume_hint);
    } else if (!wizard_ && disc_) {
        hint(pad ? "Start" : (italian() ? "Invio" : "Enter"), kChrome, t.play_hint);
    }
    if (!wizard_) hint(pad ? "LB RB" : "Q E", kChrome, page_ == Page::kSettings ? t.tabs_hint : t.pages_hint);
    hint(pad ? "B" : "Esc", IM_COL32(255, 90, 90, 255), t.back);
    hint(pad ? "A" : (italian() ? "Invio" : "Enter"), kGreen, t.select);
    hint(pad ? t.dpad : "\xE2\x86\x91\xE2\x86\x93\xE2\x86\x90\xE2\x86\x92", kChrome,
         pad && menu_has_focus_ && !wizard_ ? t.enter_hint : t.move_hint);
}

// --- in game ---

// Whether the keys and buttons that close the menu are let go: otherwise the resumed game would see them (Start
// would open its own pause menu, Enter or A would choose something).
bool closing_inputs_released(decltype(&XInputGetState) get_state) {
    for (const int key : {VK_ESCAPE, VK_RETURN, VK_SPACE, VK_F1, VK_BACK, VK_LBUTTON}) {
        if (GetAsyncKeyState(key) & 0x8000) return false;
    }
    XINPUT_STATE state;
    for (DWORD i = 0; get_state && i < 4; ++i) {
        if (get_state(i, &state) == ERROR_SUCCESS &&
            (state.Gamepad.wButtons || state.Gamepad.bLeftTrigger > 30 || state.Gamepad.bRightTrigger > 30)) {
            return false;
        }
    }
    return true;
}

// The controllers into ImGui, like its Win32 backend but for every port (the backend reads only the first, and only
// learns of a controller plugged in later from a window message the game window does not pass on): their buttons
// together. The left stick also works as a d-pad, so it moves through menus and pages, with hysteresis so a
// direction holds until the stick comes well back.
void Launcher::poll_gamepads(float now) {
    ImGuiIO& io = ImGui::GetIO();
    const bool rescan = now - pad_rescan_ > 0.5f;  // XInputGetState on an empty port is slow
    if (rescan) pad_rescan_ = now;
    XINPUT_GAMEPAD pad{};
    bool any = false;
    for (DWORD i = 0; xinput_get_state_ && i < 4; ++i) {
        if (!pad_present_[i] && !rescan) continue;
        XINPUT_STATE state{};
        pad_present_[i] = xinput_get_state_(i, &state) == ERROR_SUCCESS;
        if (!pad_present_[i]) continue;
        any = true;
        const XINPUT_GAMEPAD& g = state.Gamepad;
        pad.wButtons |= g.wButtons;
        pad.bLeftTrigger = std::max(pad.bLeftTrigger, g.bLeftTrigger);
        pad.bRightTrigger = std::max(pad.bRightTrigger, g.bRightTrigger);
        const auto larger = [](SHORT a, SHORT b) { return std::abs(static_cast<int>(a)) >= std::abs(static_cast<int>(b)) ? a : b; };
        pad.sThumbLX = larger(pad.sThumbLX, g.sThumbLX);
        pad.sThumbLY = larger(pad.sThumbLY, g.sThumbLY);
        pad.sThumbRX = larger(pad.sThumbRX, g.sThumbRX);
        pad.sThumbRY = larger(pad.sThumbRY, g.sThumbRY);
    }
    pad_connected_ = any;
    if (!any) {
        io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;  // ImGui then clears the gamepad keys
        stick_x_ = stick_y_ = 0;
        return;
    }
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    const auto axis = [](SHORT value, int& direction) {
        const float v = static_cast<float>(value) / 32767.0f;
        if (direction == 0 && v > 0.55f) direction = 1;
        else if (direction == 0 && v < -0.55f) direction = -1;
        else if (direction != 0 && std::fabs(v) < 0.35f) direction = 0;
    };
    const int was_x = stick_x_, was_y = stick_y_;
    axis(pad.sThumbLX, stick_x_);
    axis(pad.sThumbLY, stick_y_);
    // The stick presses the d-pad in pulses at a menu's pace: once when it tilts, then after kStickDelay every
    // kStickRate while it stays tilted. Held down for real, ImGui would repeat 20 times a second.
    constexpr float kStickDelay = 0.4f, kStickRate = 0.15f;
    bool pulse = false;
    if (stick_x_ != was_x || stick_y_ != was_y) {
        pulse = stick_x_ != 0 || stick_y_ != 0;
        stick_repeat_at_ = now + kStickDelay;
    } else if ((stick_x_ != 0 || stick_y_ != 0) && now >= stick_repeat_at_) {
        pulse = true;
        stick_repeat_at_ = now + kStickRate;
    }
    const auto button = [&](ImGuiKey key, WORD mask, bool also = false) { io.AddKeyEvent(key, (pad.wButtons & mask) != 0 || also); };
    const auto analog = [&](ImGuiKey key, int value, int v0, int v1) {
        const float v = std::clamp(static_cast<float>(value - v0) / static_cast<float>(v1 - v0), 0.0f, 1.0f);
        io.AddKeyAnalogEvent(key, v > 0.10f, v);
    };
    button(ImGuiKey_GamepadStart, XINPUT_GAMEPAD_START);
    button(ImGuiKey_GamepadBack, XINPUT_GAMEPAD_BACK);
    button(ImGuiKey_GamepadFaceLeft, XINPUT_GAMEPAD_X);
    button(ImGuiKey_GamepadFaceRight, XINPUT_GAMEPAD_B);
    button(ImGuiKey_GamepadFaceUp, XINPUT_GAMEPAD_Y);
    button(ImGuiKey_GamepadFaceDown, XINPUT_GAMEPAD_A);
    button(ImGuiKey_GamepadDpadLeft, XINPUT_GAMEPAD_DPAD_LEFT, pulse && stick_x_ < 0);
    button(ImGuiKey_GamepadDpadRight, XINPUT_GAMEPAD_DPAD_RIGHT, pulse && stick_x_ > 0);
    button(ImGuiKey_GamepadDpadUp, XINPUT_GAMEPAD_DPAD_UP, pulse && stick_y_ > 0);
    button(ImGuiKey_GamepadDpadDown, XINPUT_GAMEPAD_DPAD_DOWN, pulse && stick_y_ < 0);
    button(ImGuiKey_GamepadL1, XINPUT_GAMEPAD_LEFT_SHOULDER);
    button(ImGuiKey_GamepadR1, XINPUT_GAMEPAD_RIGHT_SHOULDER);
    button(ImGuiKey_GamepadL3, XINPUT_GAMEPAD_LEFT_THUMB);
    button(ImGuiKey_GamepadR3, XINPUT_GAMEPAD_RIGHT_THUMB);
    analog(ImGuiKey_GamepadL2, pad.bLeftTrigger, XINPUT_GAMEPAD_TRIGGER_THRESHOLD, 255);
    analog(ImGuiKey_GamepadR2, pad.bRightTrigger, XINPUT_GAMEPAD_TRIGGER_THRESHOLD, 255);
    constexpr int kDead = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
    analog(ImGuiKey_GamepadLStickLeft, pad.sThumbLX, -kDead, -32768);
    analog(ImGuiKey_GamepadLStickRight, pad.sThumbLX, kDead, 32767);
    analog(ImGuiKey_GamepadLStickUp, pad.sThumbLY, kDead, 32767);
    analog(ImGuiKey_GamepadLStickDown, pad.sThumbLY, -kDead, -32768);
    analog(ImGuiKey_GamepadRStickLeft, pad.sThumbRX, -kDead, -32768);
    analog(ImGuiKey_GamepadRStickRight, pad.sThumbRX, kDead, 32767);
    analog(ImGuiKey_GamepadRStickUp, pad.sThumbRY, kDead, 32767);
    analog(ImGuiKey_GamepadRStickDown, pad.sThumbRY, -kDead, -32768);
}

// A thin red frame around the last ImGui widget while it is hovered or focused, like the launcher's own buttons;
// ImGui's own navigation rectangle is off.
void Launcher::focus_ring() {
    const float hot = anim(ImGui::GetItemID(), ImGui::IsItemHovered() || ImGui::IsItemFocused());
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (hot < 0.01f) return;
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    list_->AddRect(ImVec2(a.x - px(2), a.y - px(2)), ImVec2(b.x + px(2), b.y + px(2)), fade(kRed, hot), 0, 0, px(1.5f));
}

// Xbox-style glyphs for the controller's buttons, drawn with circles and rounded boxes (no textures): the face
// buttons in their colours, bumpers and triggers, the sticks with their direction, the d-pad, Start and Back. The
// names are the bindings' and the hints' ("A", "LB", "RT", "L ←", "D-pad ↑", "Start", "Back", "Black",
// "White", "LB RB"). Returns the width used; with `draw` false it only measures.
float Launcher::pad_icon(ImDrawList* list, const char* name, ImVec2 at, float h, bool draw) {
    const std::string n = name;
    const float cy = at.y + h / 2;
    const ImU32 ink = fade(IM_COL32(6, 6, 7, 255)), chrome = fade(kChrome);
    const auto label = [&](const char* str, float cx, float size, ImU32 colour) {
        if (!draw) return;
        const ImVec2 s = measure(body_bold_, size, str);
        list->AddText(body_bold_, size, ImVec2(cx - s.x / 2, cy - s.y / 2), colour, str);
    };
    const auto circle = [&](const char* letter, ImU32 colour) {
        if (draw) list->AddCircleFilled(ImVec2(at.x + h / 2, cy), h / 2, fade(colour));
        label(letter, at.x + h / 2, h * 0.62f, ink);
        return h;
    };
    const auto bumper = [&](const char* str, float x) {  // wide and low, rounder on top
        const float w = h * 1.7f, top = cy - h * 0.34f, bottom = cy + h * 0.34f;
        if (draw) {
            list->AddRectFilled(ImVec2(x, top), ImVec2(x + w, cy + h * 0.1f), chrome, h * 0.32f, ImDrawFlags_RoundCornersTop);
            list->AddRectFilled(ImVec2(x, cy - h * 0.05f), ImVec2(x + w, bottom), chrome, h * 0.1f, ImDrawFlags_RoundCornersBottom);
        }
        label(str, x + w / 2, h * 0.5f, ink);
        return w;
    };
    const auto trigger = [&](const char* str) {  // tall, rounder on top
        const float w = h * 1.05f;
        if (draw) {
            list->AddRectFilled(ImVec2(at.x, at.y), ImVec2(at.x + w, cy + h * 0.1f), chrome, h * 0.42f, ImDrawFlags_RoundCornersTop);
            list->AddRectFilled(ImVec2(at.x, cy - h * 0.1f), ImVec2(at.x + w, at.y + h), chrome, h * 0.14f, ImDrawFlags_RoundCornersBottom);
        }
        label(str, at.x + w / 2, h * 0.5f, ink);
        return w;
    };
    // The direction at the end of the name: an arrow in UTF-8.
    ImVec2 direction(0, 0);
    if (n.size() >= 3) {
        const std::string arrow = n.substr(n.size() - 3);
        if (arrow == "\xE2\x86\x91") direction = ImVec2(0, -1);
        else if (arrow == "\xE2\x86\x93") direction = ImVec2(0, 1);
        else if (arrow == "\xE2\x86\x90") direction = ImVec2(-1, 0);
        else if (arrow == "\xE2\x86\x92") direction = ImVec2(1, 0);
    }
    if (n == "A") return circle("A", kGreen);
    if (n == "B") return circle("B", IM_COL32(255, 90, 90, 255));
    if (n == "X") return circle("X", IM_COL32(90, 150, 255, 255));
    if (n == "Y") return circle("Y", IM_COL32(255, 210, 60, 255));
    if (n == "LB" || n == "White") return bumper("LB", at.x);
    if (n == "RB" || n == "Black") return bumper("RB", at.x);
    if (n == "LB RB") {
        const float w = bumper("LB", at.x);
        return w + h * 0.25f + bumper("RB", at.x + w + h * 0.25f);
    }
    if (n == "LT" || n == "RT") return trigger(name);
    if (n == "Start" || n == "Back") {
        if (draw) {
            const ImVec2 c(at.x + h / 2, cy);
            list->AddCircleFilled(c, h / 2, chrome);
            if (n == "Start") {  // three lines
                for (int i = -1; i <= 1; ++i) {
                    list->AddLine(ImVec2(c.x - h * 0.24f, c.y + h * 0.19f * static_cast<float>(i)),
                                  ImVec2(c.x + h * 0.24f, c.y + h * 0.19f * static_cast<float>(i)), ink, h * 0.09f);
                }
            } else {  // two overlapping squares
                list->AddRect(ImVec2(c.x - h * 0.26f, c.y - h * 0.08f), ImVec2(c.x + h * 0.06f, c.y + h * 0.26f), ink, 0, 0, h * 0.08f);
                list->AddRect(ImVec2(c.x - h * 0.06f, c.y - h * 0.26f), ImVec2(c.x + h * 0.26f, c.y + h * 0.08f), ink, 0, 0, h * 0.08f);
            }
        }
        return h;
    }
    if (n.rfind("D-pad", 0) == 0 || n.rfind("Croce", 0) == 0) {  // a cross, the pressed arm in red
        if (draw) {
            const ImVec2 c(at.x + h / 2, cy);
            const float arm = h * 0.17f, reach = h * 0.5f;
            list->AddRectFilled(ImVec2(c.x - arm, c.y - reach), ImVec2(c.x + arm, c.y + reach), chrome, arm * 0.6f);
            list->AddRectFilled(ImVec2(c.x - reach, c.y - arm), ImVec2(c.x + reach, c.y + arm), chrome, arm * 0.6f);
            if (direction.x != 0 || direction.y != 0) {
                const ImVec2 tip(c.x + direction.x * reach, c.y + direction.y * reach);
                const ImVec2 lo(std::min(c.x, tip.x) - (direction.y != 0 ? arm : 0), std::min(c.y, tip.y) - (direction.x != 0 ? arm : 0));
                const ImVec2 hi(std::max(c.x, tip.x) + (direction.y != 0 ? arm : 0), std::max(c.y, tip.y) + (direction.x != 0 ? arm : 0));
                list->AddRectFilled(lo, hi, fade(kRed), arm * 0.6f);
            }
        }
        return h;
    }
    if (n == "LS" || n == "RS") {  // a stick pressed: ring, centre and letter
        const char letter[2] = {n[0], 0};
        const float letter_size = h * 0.55f;
        const ImVec2 s = measure(body_bold_, letter_size, letter);
        if (draw) {
            const ImVec2 c(at.x + h / 2, cy);
            list->AddCircle(c, h * 0.46f, chrome, 0, h * 0.09f);
            list->AddCircleFilled(c, h * 0.22f, chrome);
            list->AddCircle(c, h * 0.3f, chrome, 0, h * 0.05f);
            list->AddText(body_bold_, letter_size, ImVec2(at.x + h + h * 0.12f, cy - s.y / 2), fade(kInk), letter);
        }
        return h + h * 0.12f + s.x;
    }
    if (n.rfind("L ", 0) == 0 || n.rfind("R ", 0) == 0) {  // a stick tilted in its direction, with its letter
        const char letter[2] = {n[0], 0};
        const float letter_size = h * 0.55f;
        const ImVec2 s = measure(body_bold_, letter_size, letter);
        if (draw) {
            const ImVec2 c(at.x + h / 2, cy);
            list->AddCircle(c, h * 0.46f, chrome, 0, h * 0.09f);
            list->AddCircleFilled(ImVec2(c.x + direction.x * h * 0.17f, c.y + direction.y * h * 0.17f), h * 0.22f, chrome);
            list->AddText(body_bold_, letter_size, ImVec2(at.x + h + h * 0.12f, cy - s.y / 2), fade(kInk), letter);
        }
        return h + h * 0.12f + s.x;
    }
    // Anything else: the name in a chrome pill.
    const ImVec2 s = measure(body_bold_, h * 0.62f, name);
    const float w = std::max(h * 1.1f, s.x + h * 0.5f);
    if (draw) list->AddRectFilled(at, ImVec2(at.x + w, at.y + h), chrome, h / 2);
    label(name, at.x + w / 2, h * 0.62f, ink);
    return w;
}

void Launcher::resume() {
    if (resuming_) return;
    resuming_ = true;
    resume_since_ = now_;
}

// The game stops while the menu is open; so do the controllers' motors.
void Launcher::stop_rumble() {
    if (!xinput_set_state_) return;
    XINPUT_VIBRATION still{};
    for (DWORD i = 0; i < 4; ++i) xinput_set_state_(i, &still);
}

void Launcher::page_paused() {
    const Texts& t = T();
    title(t.paused);
    lead(t.paused_lead);
    status(pad_connected_, pad_connected_ ? t.pad_on : t.pad_off, pad_connected_ ? t.pad_on_sub : t.pad_off_sub, nullptr, nullptr);
    char summary[160];
    const int preset = quality_preset();
    const char* quality = preset == 0 ? t.original : preset == 1 ? t.balanced : preset == 2 ? t.high : t.custom;
    std::snprintf(summary, sizeof(summary), "%s \xC2\xB7 %.0f fps \xC2\xB7 %s %s", setting("widescreen") != 0 ? "16:9" : "4:3",
                  setting("frame_rate") >= 60 ? 60.0f : 30.0f, t.quality, quality);
    if (status(true, summary, setting("effects") != 0 ? t.effects_sub : t.effects, t.edit, "##edit")) {
        page_ = Page::kSettings;
        page_since_ = now_;
    }
    ImGui::SetCursorScreenPos(ImVec2(origin_.x, origin_.y + cursor_));
    if (big_button(t.back_to_game, "##resume", false)) resume();
}

void Launcher::page_exit() {
    const Texts& t = T();
    title(t.exit_game);
    lead(t.exit_lead);
    if (big_button(t.exit_now, "##exit", false)) {
        log_line("game menu: exit");
        ExitProcess(0);
    }
}

void Launcher::frame_in_game(float now) {
    const Texts& t = T();
    const ImVec2 screen(static_cast<float>(width_), static_cast<float>(height_));
    const float since = now - start_time_;

    // The paused game, darker, and darkest under the menu on the left.
    const float dim = ease_out(since / 0.3f);
    ImDrawList* back = ImGui::GetBackgroundDrawList();
    back->AddRectFilled(ImVec2(0, 0), screen, IM_COL32(0, 0, 0, static_cast<int>(120 * dim)));
    const ImU32 shade = IM_COL32(4, 4, 6, static_cast<int>(210 * dim)), clear = IM_COL32(4, 4, 6, 0);
    back->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(screen.x * 0.55f, screen.y), shade, clear, clear, shade);

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(screen);
    ImGui::Begin("##game_menu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    list_ = ImGui::GetWindowDrawList();
    // The launcher's entrance, twice as quick.
    header(since * 2);
    const char* labels[] = {t.resume, t.settings, t.controls, t.quit};
    const Page targets[] = {Page::kPlay, Page::kSettings, Page::kControls, Page::kQuit};
    side_menu(now, since * 2, labels, targets, 4);
    panel(now, since * 2);
    bottom_bar(since * 2);
    g_alpha = 1;
    ImGui::End();

    // Back: from the page to the menu, then to the pause page, then to the game. Start resumes from anywhere.
    if (rebinding_ < 0 && !resuming_ && cancel_frame_ != ImGui::GetFrameCount()) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
            if (!menu_has_focus_) {
                back_to_menu();
            } else if (page_ != Page::kPlay) {
                page_ = Page::kPlay;
                menu_cursor_ = 0;
                page_since_ = now;
            } else {
                resume();
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart)) resume();
    }
    leave_page_on_left();
}

constexpr float kMenuHintEnd = 10;  // seconds after the game's first frame

// "F1 · Back+Start: in-game menu" in the top left corner while the game starts, fading in and out.
void Launcher::draw_menu_hint(float now) {
    g_alpha = std::clamp(std::min(now - 1.0f, kMenuHintEnd - now) / 0.6f, 0.0f, 1.0f);
    const float size = std::max(15.0f, px(20)), pad = size * 0.45f;
    const char* label = T().menu_key;
    ImDrawList* list = ImGui::GetForegroundDrawList();
    const ImVec2 f1_size = body_bold_->CalcTextSizeA(size, FLT_MAX, 0, "F1");
    const ImVec2 or_size = body_->CalcTextSizeA(size, FLT_MAX, 0, "\xC2\xB7");
    const ImVec2 plus_size = body_->CalcTextSizeA(size, FLT_MAX, 0, "+");
    const ImVec2 label_size = body_->CalcTextSizeA(size, FLT_MAX, 0, label);
    const float icon_h = size * 1.15f;
    const float back_w = pad_icon(list, "Back", ImVec2(0, 0), icon_h, false), start_w = pad_icon(list, "Start", ImVec2(0, 0), icon_h, false);
    const float keys_w = f1_size.x + pad * 1.2f + pad * 0.8f + or_size.x + pad * 0.8f + back_w + pad * 0.4f + plus_size.x + pad * 0.4f + start_w;
    const ImVec2 a(px(32), px(28));
    const ImVec2 b(a.x + keys_w + label_size.x + pad * 5, a.y + f1_size.y + pad * 2);
    list->AddRectFilled(a, b, fade(IM_COL32(8, 8, 10, 190)), size * 0.25f);
    list->AddRectFilled(a, ImVec2(a.x + size * 0.18f, b.y), fade(kRed));
    float x = a.x + pad * 1.5f;
    const float text_y = a.y + pad, icon_y = a.y + pad + f1_size.y / 2 - icon_h / 2;
    list->AddRectFilled(ImVec2(x, a.y + pad * 0.6f), ImVec2(x + f1_size.x + pad * 1.2f, b.y - pad * 0.6f), fade(kChrome), size * 0.2f);
    list->AddText(body_bold_, size, ImVec2(x + pad * 0.6f, text_y), fade(IM_COL32(6, 6, 7, 255)), "F1");
    x += f1_size.x + pad * 1.2f + pad * 0.8f;
    list->AddText(body_, size, ImVec2(x, text_y), fade(kDim), "\xC2\xB7");
    x += or_size.x + pad * 0.8f;
    x += pad_icon(list, "Back", ImVec2(x, icon_y), icon_h) + pad * 0.4f;
    list->AddText(body_, size, ImVec2(x, text_y), fade(kInk), "+");
    x += plus_size.x + pad * 0.4f;
    x += pad_icon(list, "Start", ImVec2(x, icon_y), icon_h);
    list->AddText(body_, size, ImVec2(x + pad * 1.4f, text_y), fade(kInk), label);
    g_alpha = 1;
}

// Frames per second in the top right corner (setting fps_counter): the game's frames, not the presents.
void Launcher::draw_frame_counter(float now) {
    const uint32_t frames = nv2a_frames_shown();
    if (counted_since_ < 0 || now < counted_since_) {
        counted_since_ = now;
        counted_frames_ = frames;
    } else if (now - counted_since_ >= 0.5f) {
        frames_per_second_ = static_cast<float>(frames - counted_frames_) / (now - counted_since_);
        counted_since_ = now;
        counted_frames_ = frames;
    }
    char label[24];
    std::snprintf(label, sizeof(label), "%.0f FPS", frames_per_second_);
    const float size = std::max(16.0f, px(22));
    const ImVec2 extent = body_bold_->CalcTextSizeA(size, FLT_MAX, 0, label);
    const ImVec2 at(static_cast<float>(width_) - extent.x - size, size * 0.6f);
    ImDrawList* list = ImGui::GetForegroundDrawList();
    list->AddRectFilled(ImVec2(at.x - size * 0.4f, at.y - size * 0.2f),
                        ImVec2(at.x + extent.x + size * 0.4f, at.y + extent.y + size * 0.2f), IM_COL32(0, 0, 0, 150), size * 0.2f);
    list->AddText(body_bold_, size, at, kInk, label);
}

void Launcher::overlay(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* target, UINT width,
                       UINT height) {
    if (!device_) {  // the first frame: ImGui on the renderer's device
        std::lock_guard<std::recursive_mutex> lock(g_ui_mutex);
        device_ = device;
        context_ = context;
        QueryPerformanceCounter(&clock_start_);
        start_imgui();
        if (!disc_path_.empty()) start_art_loader(disc_path_);
        log_line("game menu: ready (F1, or Back+Start on a controller)");
    }
    LARGE_INTEGER frequency, counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    const float now = static_cast<float>(counter.QuadPart - clock_start_.QuadPart) / static_cast<float>(frequency.QuadPart);
    now_ = now;

    if (toggle_requested_.exchange(false)) {
        if (visible_) {
            resume();
        } else {
            open_requested_ = true;
        }
    }
    if (open_requested_.exchange(false) && !visible_) {
        visible_ = true;
        resuming_ = false;
        page_ = Page::kPlay;
        page_since_ = start_time_ = now;
        rebinding_ = -1;
        menu_has_focus_ = true;
        menu_cursor_ = 0;
        game_pause(true);
        stop_rumble();
        std::lock_guard<std::recursive_mutex> lock(g_ui_mutex);
        ImGui::GetIO().ClearInputKeys();  // the key that opened the menu, and keys held in the game
        log_line("game menu: open");
    }
    if (visible_ && resuming_ && (closing_inputs_released(xinput_get_state_) || now - resume_since_ > 1.5f)) {
        visible_ = false;
        resuming_ = false;
        g_capturing = false;
        counted_since_ = -1;
        game_pause(false);
        log_line("game menu: closed");
    }
    const bool counter_on = setting("fps_counter") != 0;
    const bool hint_on = now < kMenuHintEnd;  // how to open the menu, for the first seconds of the game
    if (!visible_ && !counter_on && !hint_on) return;

    width_ = width;
    height_ = height;
    scale_ = std::max(0.65f, std::min(width / 1600.0f, height / 900.0f));
    upload_art();
    {
        std::lock_guard<std::recursive_mutex> lock(g_ui_mutex);
        configure_navigation();
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        poll_gamepads(now);
        ImGui::NewFrame();
        if (visible_) {
            frame_in_game(now);
        } else {
            if (counter_on) draw_frame_counter(now);
            if (hint_on) draw_menu_hint(now);
        }
        ImGui::Render();
    }
    context->OMSetRenderTargets(1, &target, nullptr);
    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    context->RSSetViewports(1, &viewport);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
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
            std::lock_guard<std::recursive_mutex> lock(g_ui_mutex);
            configure_navigation();
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            poll_gamepads(time);
            ImGui::NewFrame();
            if (start_time_ == 0) start_time_ = page_since_ = time;
            frame(time, dt);
            ImGui::Render();
        }
        draw_background(time);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        swap_chain_->Present(1, 0);
        if (choose_disc_requested_) {  // outside the lock: see choose_disc
            choose_disc_requested_ = false;
            run_disc_dialog();
        }
    }
    log_line("launcher: %s", play_ ? "play" : "quit");
    stop_graphics();
    return play_ ? std::move(disc_) : std::nullopt;
}

Launcher* g_game_menu = nullptr;  // start_game_menu; lives as long as the game

// The game window's messages: F1 opens and closes the menu, the window going to the background opens it (setting
// pause_in_background); while it is open, the launcher's input handling.
bool game_window_hook(HWND window, UINT message, WPARAM wparam, LPARAM lparam, LRESULT* result) {
    if (message == WM_KEYDOWN && wparam == VK_F1 && !g_capturing) {
        if (!(lparam & (1 << 30))) g_game_menu->request_toggle();  // not on auto-repeat
        *result = 0;
        return true;
    }
    if (message == WM_ACTIVATEAPP && !wparam && setting("pause_in_background") != 0) g_game_menu->request_open();
    return g_game_menu->visible() && window_hook(window, message, wparam, lparam, result);
}

void draw_game_menu(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* target, uint32_t width,
                    uint32_t height) {
    g_game_menu->overlay(device, context, target, width, height);
}

}  // namespace

std::optional<GameDisc> run_launcher(HWND window, const std::filesystem::path& exe_directory) {
    Launcher launcher(window, exe_directory);
    return launcher.run();
}

void start_game_menu(HWND window, const std::filesystem::path& disc_path) {
    g_game_menu = new Launcher(window, program_directory(), true);
    g_game_menu->set_disc_path(disc_path);
    nv2a_set_window_hook(game_window_hook);
    renderer_set_overlay(draw_game_menu);
}

void game_menu_open() {
    if (g_game_menu) g_game_menu->request_open();
}

}  // namespace mc3::pc
