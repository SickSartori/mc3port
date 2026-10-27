// Xbox controllers replaced at the XPP API boundary.
//
// The game talks to its controllers only through a handful of XPP functions (found from its input code at 0x84B40
// and 0x84C00, see docs/ANALISI_XBE.md). XInitDevices is skipped (hle_patches.cpp), so there is no USB stack; these
// functions are redirected here and served from XInput. A device type ("XPP_DEVICE_TYPE") is three port masks:
// connected now, ports that changed since the last query, connected at the last query. XGetDevices and
// XGetDeviceChanges keep the original logic on those masks, refreshing the gamepad masks from the host first.

#include "pc/input_hle.h"

#include <windows.h>
#include <xinput.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "pc/exec_memory.h"
#include "pc/keyboard_bindings.h"
#include "pc/launcher_ui.h"
#include "pc/log.h"

namespace mc3::pc {
namespace {

constexpr uint32_t kPorts = 4;
constexpr uint32_t kGamepadType = 0x0043E994;  // XDEVICE_TYPE_GAMEPAD

constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kErrorIoPending = 997;
constexpr uint32_t kErrorDeviceNotConnected = 1167;

// Xbox XINPUT_GAMEPAD: digital buttons (same bits as XInput's low byte), then 8 analog buttons.
enum : uint32_t { kAnalogA, kAnalogB, kAnalogX, kAnalogY, kAnalogBlack, kAnalogWhite, kAnalogLeftTrigger,
                  kAnalogRightTrigger };
constexpr uint16_t kDpadUp = 0x01, kDpadDown = 0x02, kDpadLeft = 0x04, kDpadRight = 0x08, kStart = 0x10,
                   kBack = 0x20;

#pragma pack(push, 1)
struct XboxGamepad {
    uint16_t buttons;
    uint8_t analog[8];
    int16_t thumb_lx, thumb_ly, thumb_rx, thumb_ry;
};
struct XboxInputState {
    uint32_t packet_number;
    XboxGamepad gamepad;
};
// XINPUT_CAPABILITIES: the output side is only the rumble motors. The game passes a 28-byte stack buffer, so a
// bigger structure overwrites its return address.
struct XboxCapabilities {
    uint8_t sub_type;
    uint16_t reserved;
    XboxGamepad in;
    uint16_t left_motor;
    uint16_t right_motor;
};
static_assert(sizeof(XboxCapabilities) <= 28);
struct XboxFeedback {
    uint32_t status;
    uint32_t event;
    uint8_t reserved[58];
    uint16_t left_motor;
    uint16_t right_motor;
};
struct DeviceType {
    uint32_t connected;
    uint32_t changed;
    uint32_t previous;
};
#pragma pack(pop)

constexpr uint8_t kSubTypeGamepad = 0x01;

struct Port {
    uint32_t magic = 0x50414458;  // handles point here
    uint32_t index = 0;
    bool open = false;
    XboxGamepad last{};
    uint32_t packet_number = 0;
};

// MC3_INPUT_SCRIPT entries: button pressed from `at` for a short time.
struct ScriptedPress {
    ULONGLONG at_ms;
    ULONGLONG until_ms;  // "<start>-<end>:<button>" holds it; "<start>:<button>" taps it for 200 ms
    std::string button;  // "p2a", "p2start"...: the same buttons on controller 2 (up to p4)
    bool logged = false;
    uint32_t port = 0;
    // "@<file>[#n]+<seconds>[~<hold>]:<button>": relative to the n-th time the game opens a file whose path
    // contains <file> (menus and loads open their files: robust against timing differences between runs).
    std::string event;
    size_t occurrence = 1;
    ULONGLONG offset_ms = 0, hold_ms = 200;
};

// When the files named by "@" entries were opened (ms since start), per name.
std::vector<std::pair<std::string, std::vector<ULONGLONG>>> g_events;

using XInputGetStateFunction = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XInputSetStateFunction = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);

std::mutex g_mutex;
Port g_ports[kPorts];
XInputGetStateFunction g_xinput_get_state = nullptr;
XInputSetStateFunction g_xinput_set_state = nullptr;
ULONGLONG g_next_scan_ms[kPorts] = {};  // XInputGetState on an empty port is slow: look again once a second
bool g_host_connected[kPorts] = {};
std::vector<ScriptedPress> g_script;
uint32_t g_script_ports = 0;  // controllers the script presses on (a virtual pad is connected for each)
ULONGLONG g_start_ms = 0;

void load_xinput() {
    HMODULE module = LoadLibraryW(L"xinput1_4.dll");
    if (!module) module = LoadLibraryW(L"xinput9_1_0.dll");
    if (!module) {
        log_line("input: XInput not available, only the keyboard works");
        return;
    }
    g_xinput_get_state = reinterpret_cast<XInputGetStateFunction>(GetProcAddress(module, "XInputGetState"));
    g_xinput_set_state = reinterpret_cast<XInputSetStateFunction>(GetProcAddress(module, "XInputSetState"));
}

void load_script() {
    std::vector<char> value(16384);
    if (!GetEnvironmentVariableA("MC3_INPUT_SCRIPT", value.data(), static_cast<DWORD>(value.size()))) return;
    const std::string text = value.data();
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find(',', start);
        if (end == std::string::npos) end = text.size();
        const std::string entry = text.substr(start, end - start);
        const size_t colon = entry.find(':');
        if (colon != std::string::npos && entry[0] == '@') {
            // @name[#n]+offset[~hold]:button
            ScriptedPress press{~0ull, ~0ull, entry.substr(colon + 1)};
            const std::string when = entry.substr(1, colon - 1);
            const size_t plus = when.find('+');
            std::string name = when.substr(0, plus);
            const size_t hash = name.find('#');
            if (hash != std::string::npos) {
                press.occurrence = std::max<size_t>(1, std::strtoul(name.c_str() + hash + 1, nullptr, 10));
                name.resize(hash);
            }
            for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            press.event = name;
            if (plus != std::string::npos) {
                const std::string rest = when.substr(plus + 1);
                const size_t tilde = rest.find('~');
                press.offset_ms = static_cast<ULONGLONG>(std::atof(rest.substr(0, tilde).c_str()) * 1000);
                if (tilde != std::string::npos) press.hold_ms = static_cast<ULONGLONG>(std::atof(rest.c_str() + tilde + 1) * 1000);
            }
            if (std::none_of(g_events.begin(), g_events.end(), [&](const auto& e) { return e.first == name; })) {
                g_events.push_back({name, {}});
            }
            g_script.push_back(press);
        } else if (colon != std::string::npos) {
            const std::string when = entry.substr(0, colon);
            const size_t dash = when.find('-');
            const auto at = static_cast<ULONGLONG>(std::atof(when.substr(0, dash).c_str()) * 1000);
            const ULONGLONG until =
                dash == std::string::npos ? at + 200 : static_cast<ULONGLONG>(std::atof(when.substr(dash + 1).c_str()) * 1000);
            g_script.push_back({at, until, entry.substr(colon + 1)});
        }
        start = end + 1;
    }
    for (ScriptedPress& press : g_script) {
        if (press.button.size() > 2 && press.button[0] == 'p' && press.button[1] >= '1' && press.button[1] <= '4') {
            press.port = static_cast<uint32_t>(press.button[1] - '1');
            press.button.erase(0, 2);
            g_script_ports |= 1u << press.port;
        }
    }
    g_script_ports |= 1;  // controller 1 is always there
    log_line("input: %zu scripted presses (controllers %X)", g_script.size(), g_script_ports);
}

bool game_has_focus() {
    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    return process == GetCurrentProcessId();
}

bool key(int virtual_key) { return (GetAsyncKeyState(virtual_key) & 0x8000) != 0; }

void press(XboxGamepad& pad, const std::string& button) {
    if (button == "start") pad.buttons |= kStart;
    else if (button == "back") pad.buttons |= kBack;
    else if (button == "up") pad.buttons |= kDpadUp;
    else if (button == "down") pad.buttons |= kDpadDown;
    else if (button == "left") pad.buttons |= kDpadLeft;
    else if (button == "right") pad.buttons |= kDpadRight;
    else if (button == "a") pad.analog[kAnalogA] = 255;
    else if (button == "b") pad.analog[kAnalogB] = 255;
    else if (button == "x") pad.analog[kAnalogX] = 255;
    else if (button == "y") pad.analog[kAnalogY] = 255;
    else if (button == "black") pad.analog[kAnalogBlack] = 255;
    else if (button == "white") pad.analog[kAnalogWhite] = 255;
    else if (button == "lt") pad.analog[kAnalogLeftTrigger] = 255;
    else if (button == "rt") pad.analog[kAnalogRightTrigger] = 255;
    else if (button == "sl") pad.thumb_lx = -32768;  // steer with the left stick
    else if (button == "sr") pad.thumb_lx = 32767;
}

// The test script's presses on controller `port` (added to a real pad's state when one is connected).
void apply_script(XboxGamepad& pad, uint32_t port) {
    if (!g_script.empty()) {
        const ULONGLONG now = GetTickCount64() - g_start_ms;
        for (ScriptedPress& entry : g_script) {
            if (!entry.event.empty() && entry.at_ms == ~0ull) {
                for (const auto& [name, times] : g_events) {
                    if (name == entry.event && times.size() >= entry.occurrence) {
                        entry.at_ms = times[entry.occurrence - 1] + entry.offset_ms;
                        entry.until_ms = entry.at_ms + entry.hold_ms;
                    }
                }
            }
            if (entry.port == port && now >= entry.at_ms && now < entry.until_ms) {
                press(pad, entry.button);
                if (!entry.logged) {
                    log_line("input: script presses %s on controller %u at %.1f s", entry.button.c_str(), port + 1,
                             now / 1000.0);
                }
                entry.logged = true;
            }
        }
    }
}

// Controller 1 without a pad: the keyboard (only while the game window is in front) and the test script.
bool virtual_pad(XboxGamepad& pad) {
    pad = {};
    apply_script(pad, 0);
    if (game_has_focus()) {
        const auto down = [](PadInput input) { return key(bound_key(input)); };
        if (down(PadInput::kDpadUp)) pad.buttons |= kDpadUp;
        if (down(PadInput::kDpadDown)) pad.buttons |= kDpadDown;
        if (down(PadInput::kDpadLeft)) pad.buttons |= kDpadLeft;
        if (down(PadInput::kDpadRight)) pad.buttons |= kDpadRight;
        if (down(PadInput::kStart) && !(bound_key(PadInput::kStart) == VK_RETURN && key(VK_MENU))) {
            pad.buttons |= kStart;  // Alt+Enter is full screen
        }
        if (down(PadInput::kBack)) pad.buttons |= kBack;
        if (down(PadInput::kA)) pad.analog[kAnalogA] = 255;
        if (down(PadInput::kB)) pad.analog[kAnalogB] = 255;
        if (down(PadInput::kX)) pad.analog[kAnalogX] = 255;
        if (down(PadInput::kY)) pad.analog[kAnalogY] = 255;
        if (down(PadInput::kBlack)) pad.analog[kAnalogBlack] = 255;
        if (down(PadInput::kWhite)) pad.analog[kAnalogWhite] = 255;
        if (down(PadInput::kLeftTrigger)) pad.analog[kAnalogLeftTrigger] = 255;
        if (down(PadInput::kRightTrigger)) pad.analog[kAnalogRightTrigger] = 255;
        if (down(PadInput::kSteerLeft)) pad.thumb_lx = -32768;
        if (down(PadInput::kSteerRight)) pad.thumb_lx = 32767;
        if (down(PadInput::kLookLeft)) pad.thumb_rx = -32768;
        if (down(PadInput::kLookRight)) pad.thumb_rx = 32767;
        if (down(PadInput::kLookUp)) pad.thumb_ry = 32767;
        if (down(PadInput::kLookDown)) pad.thumb_ry = -32768;
    }
    return true;  // controller 1 is always there
}

XboxGamepad from_xinput(const XINPUT_GAMEPAD& in) {
    XboxGamepad pad{};
    pad.buttons = in.wButtons & 0xFF;  // d-pad, start, back, thumb clicks
    pad.analog[kAnalogA] = (in.wButtons & XINPUT_GAMEPAD_A) ? 255 : 0;
    pad.analog[kAnalogB] = (in.wButtons & XINPUT_GAMEPAD_B) ? 255 : 0;
    pad.analog[kAnalogX] = (in.wButtons & XINPUT_GAMEPAD_X) ? 255 : 0;
    pad.analog[kAnalogY] = (in.wButtons & XINPUT_GAMEPAD_Y) ? 255 : 0;
    pad.analog[kAnalogBlack] = (in.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0;
    pad.analog[kAnalogWhite] = (in.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;
    pad.analog[kAnalogLeftTrigger] = in.bLeftTrigger;
    pad.analog[kAnalogRightTrigger] = in.bRightTrigger;
    pad.thumb_lx = in.sThumbLX;
    pad.thumb_ly = in.sThumbLY;
    pad.thumb_rx = in.sThumbRX;
    pad.thumb_ry = in.sThumbRY;
    return pad;
}

// Reads port `index` from the host. Port 0 falls back to the virtual pad; other ports to the script's presses when
// the script uses them (to test split screen without a second controller).
bool read_host(uint32_t index, XboxGamepad& pad) {
    const ULONGLONG now = GetTickCount64();
    if (g_xinput_get_state && (g_host_connected[index] || now >= g_next_scan_ms[index])) {
        XINPUT_STATE state{};
        g_host_connected[index] = g_xinput_get_state(index, &state) == ERROR_SUCCESS;
        if (!g_host_connected[index]) g_next_scan_ms[index] = now + 1000;
        if (g_host_connected[index]) {
            // Back+Start opens the in-game menu; the game sees neither.
            constexpr WORD kMenuButtons = XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START;
            if ((state.Gamepad.wButtons & kMenuButtons) == kMenuButtons) {
                state.Gamepad.wButtons &= ~kMenuButtons;
                game_menu_open();
            }
            pad = from_xinput(state.Gamepad);
            apply_script(pad, index);
            return true;
        }
    }
    if (index == 0) return virtual_pad(pad);
    if (!(g_script_ports & (1u << index))) return false;
    pad = {};
    apply_script(pad, index);
    return true;
}

uint32_t host_mask() {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < kPorts; ++i) {
        XboxGamepad pad;
        if (read_host(i, pad)) mask |= 1u << i;
    }
    return mask;
}

void refresh(DeviceType* type) {
    if (address_of(type) != kGamepadType) return;
    const uint32_t mask = host_mask();
    if (mask != type->connected) {
        log_line("input: controllers connected %X (were %X)", mask, type->connected);
        type->changed |= mask ^ type->connected;
        type->connected = mask;
    }
}

Port* port_of(uint32_t handle) {
    for (Port& port : g_ports) {
        if (address_of(&port) == handle) return &port;
    }
    return nullptr;
}

// --- the XPP functions (stdcall, same prototypes) ---

uint32_t __stdcall XGetDevices(DeviceType* type) {
    std::lock_guard<std::mutex> lock(g_mutex);
    refresh(type);
    type->changed = 0;
    type->previous = type->connected;
    return type->connected;
}

uint32_t __stdcall XGetDeviceChanges(DeviceType* type, uint32_t* insertions, uint32_t* removals) {
    std::lock_guard<std::mutex> lock(g_mutex);
    refresh(type);
    if (!type->changed) {
        *insertions = *removals = 0;
        return 0;
    }
    // A port that changed but is connected before and after was unplugged and plugged back: report both.
    const uint32_t replugged = type->changed & type->previous & type->connected;
    *insertions = (type->connected & ~type->previous) | replugged;
    *removals = (~type->connected & type->previous) | replugged;
    type->changed = 0;
    type->previous = type->connected;
    return (*insertions | *removals) != 0;
}

uint32_t __stdcall XInputOpen(DeviceType* type, uint32_t port, uint32_t /*slot*/, void* /*polling*/) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (address_of(type) != kGamepadType || port >= kPorts) return 0;
    g_ports[port].index = port;
    g_ports[port].open = true;
    log_line("input: controller %u opened", port + 1);
    return address_of(&g_ports[port]);
}

void __stdcall XInputClose(uint32_t handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (Port* port = port_of(handle)) port->open = false;
}

uint32_t __stdcall XInputGetState(uint32_t handle, XboxInputState* state) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Port* port = port_of(handle);
    XboxGamepad pad{};
    if (!port || !read_host(port->index, pad)) return kErrorDeviceNotConnected;
    if (std::memcmp(&pad, &port->last, sizeof(pad)) != 0) {
        port->last = pad;
        ++port->packet_number;
    }
    state->packet_number = port->packet_number;
    state->gamepad = pad;
    return kErrorSuccess;
}

uint32_t __stdcall XInputSetState(uint32_t handle, XboxFeedback* feedback) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Port* port = port_of(handle);
    if (!port) return kErrorDeviceNotConnected;
    if (g_xinput_set_state && g_host_connected[port->index]) {
        XINPUT_VIBRATION vibration{feedback->left_motor, feedback->right_motor};
        g_xinput_set_state(port->index, &vibration);
    }
    // The real call completes asynchronously; this one is already done when the game looks.
    feedback->status = kErrorSuccess;
    if (feedback->event) {
        static bool logged = false;
        if (!logged) log_line("input: XInputSetState with a completion event, not signalled");
        logged = true;
    }
    return kErrorIoPending;
}

uint32_t __stdcall XInputGetCapabilities(uint32_t handle, XboxCapabilities* capabilities) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!port_of(handle)) return kErrorDeviceNotConnected;
    std::memset(capabilities, 0xFF, sizeof(*capabilities));  // every input and output is supported
    capabilities->sub_type = kSubTypeGamepad;
    capabilities->reserved = 0;
    return kErrorSuccess;
}

struct Redirect {
    uint32_t address;
    const void* function;
    const char* name;
};

#define R(address, function) {address, reinterpret_cast<const void*>(&function), #function}

const Redirect kRedirects[] = {
    R(0x004409F5, XGetDevices),
    R(0x00440A17, XGetDeviceChanges),
    R(0x00441D4D, XInputOpen),
    R(0x00441DA3, XInputClose),
    R(0x00441DAF, XInputGetCapabilities),
    R(0x00441F87, XInputGetState),
    R(0x00441FFA, XInputSetState),
};

#undef R

}  // namespace

void input_note_file_opened(const std::string& path) {
    if (g_events.empty()) return;
    std::string lower = path;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::lock_guard<std::mutex> lock(g_mutex);
    const ULONGLONG now = GetTickCount64() - g_start_ms;
    for (auto& [name, times] : g_events) {
        if (lower.find(name) != std::string::npos) times.push_back(now);
    }
}

void install_input_hle(const Xbe& xbe) {
    constexpr uint32_t kTitleId = 0x54540079;
    if (xbe.title_id() != kTitleId) {
        log_line("input: not the XBE these addresses were taken from; controllers are not connected");
        return;
    }
    load_xinput();
    load_script();
    g_start_ms = GetTickCount64();
    for (const Redirect& redirect : kRedirects) {
        auto* code = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(redirect.address));
        code[0] = 0xE9;  // jmp rel32
        const uint32_t relative = address_of(redirect.function) - (redirect.address + 5);
        std::memcpy(code + 1, &relative, sizeof(relative));
    }
    log_line("input: %u XPP entry points redirected to XInput and the keyboard",
             static_cast<uint32_t>(sizeof(kRedirects) / sizeof(kRedirects[0])));
}

}  // namespace mc3::pc
