// Controllers through SDL3, in XInput's shape (see gamepad.h).

#include "pc/gamepad.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <mutex>

#include "pc/log.h"

namespace mc3::pc {
namespace {

constexpr DWORD kSlots = 4;

std::mutex g_mutex;
bool g_ready = false;
SDL_Gamepad* g_slots[kSlots] = {};

// Closes the pads that went away and gives free slots to the pads that appeared.
void refresh_slots() {
    SDL_UpdateGamepads();  // with SDL's events off, this is what refreshes the buttons
    for (DWORD slot = 0; slot < kSlots; ++slot) {
        if (g_slots[slot] && !SDL_GamepadConnected(g_slots[slot])) {
            log_line("input: controller %lu unplugged", slot + 1);
            SDL_CloseGamepad(g_slots[slot]);
            g_slots[slot] = nullptr;
        }
    }
    // New pads once a second: the game reads all four ports every frame, and listing them each time allocated.
    static ULONGLONG next_scan = 0;
    const ULONGLONG now = GetTickCount64();
    if (now < next_scan) return;
    next_scan = now + 1000;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i) {
        bool known = false;
        for (SDL_Gamepad* pad : g_slots) known = known || (pad && SDL_GetGamepadID(pad) == ids[i]);
        if (known) continue;
        for (DWORD slot = 0; slot < kSlots; ++slot) {
            if (g_slots[slot]) continue;
            g_slots[slot] = SDL_OpenGamepad(ids[i]);
            if (g_slots[slot]) log_line("input: controller %lu: %s", slot + 1, SDL_GetGamepadName(g_slots[slot]));
            break;
        }
    }
    SDL_free(ids);
}

// SDL's Y axes point down, XInput's up; triggers go 0..32767 in SDL and 0..255 in XInput.
SHORT stick(SDL_Gamepad* pad, SDL_GamepadAxis axis, bool flip) {
    const int value = SDL_GetGamepadAxis(pad, axis);
    return static_cast<SHORT>(flip ? -std::max(value, -32767) : value);
}
BYTE trigger(SDL_Gamepad* pad, SDL_GamepadAxis axis) {
    return static_cast<BYTE>(std::max<int>(SDL_GetGamepadAxis(pad, axis), 0) >> 7);
}

constexpr struct { SDL_GamepadButton sdl; WORD xinput; } kButtons[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, XINPUT_GAMEPAD_A},
    {SDL_GAMEPAD_BUTTON_EAST, XINPUT_GAMEPAD_B},
    {SDL_GAMEPAD_BUTTON_WEST, XINPUT_GAMEPAD_X},
    {SDL_GAMEPAD_BUTTON_NORTH, XINPUT_GAMEPAD_Y},
    {SDL_GAMEPAD_BUTTON_BACK, XINPUT_GAMEPAD_BACK},
    {SDL_GAMEPAD_BUTTON_START, XINPUT_GAMEPAD_START},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, XINPUT_GAMEPAD_LEFT_THUMB},
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, XINPUT_GAMEPAD_RIGHT_THUMB},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT},
};

}  // namespace

bool gamepad_init() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_ready) return true;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");  // the game window is not SDL's
    g_ready = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (!g_ready) {
        log_line("input: SDL gamepad init failed: %s", SDL_GetError());
        return false;
    }
    // Nobody reads SDL's event queue here (the state is polled); hot plugging does not go through it.
    SDL_SetJoystickEventsEnabled(false);
    SDL_SetGamepadEventsEnabled(false);
    return true;
}

DWORD WINAPI gamepad_get_state(DWORD slot, XINPUT_STATE* state) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ready || slot >= kSlots) return ERROR_DEVICE_NOT_CONNECTED;
    refresh_slots();
    SDL_Gamepad* pad = g_slots[slot];
    if (!pad) return ERROR_DEVICE_NOT_CONNECTED;
    *state = {};
    XINPUT_GAMEPAD& g = state->Gamepad;
    for (const auto& button : kButtons) {
        if (SDL_GetGamepadButton(pad, button.sdl)) g.wButtons |= button.xinput;
    }
    g.bLeftTrigger = trigger(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    g.bRightTrigger = trigger(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    g.sThumbLX = stick(pad, SDL_GAMEPAD_AXIS_LEFTX, false);
    g.sThumbLY = stick(pad, SDL_GAMEPAD_AXIS_LEFTY, true);
    g.sThumbRX = stick(pad, SDL_GAMEPAD_AXIS_RIGHTX, false);
    g.sThumbRY = stick(pad, SDL_GAMEPAD_AXIS_RIGHTY, true);
    return ERROR_SUCCESS;
}

DWORD WINAPI gamepad_set_state(DWORD slot, XINPUT_VIBRATION* vibration) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ready || slot >= kSlots || !g_slots[slot]) return ERROR_DEVICE_NOT_CONNECTED;
    // shortcut: XInput rumbles until told otherwise, SDL for at most 65 s; the game refreshes it far more often.
    SDL_RumbleGamepad(g_slots[slot], vibration->wLeftMotorSpeed, vibration->wRightMotorSpeed, 0xFFFF);
    return ERROR_SUCCESS;
}

}  // namespace mc3::pc
