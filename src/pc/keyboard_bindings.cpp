#include "pc/keyboard_bindings.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

#include "pc/settings.h"

namespace mc3::pc {
namespace {

const KeyBinding kBindings[kPadInputs] = {
    {PadInput::kRightTrigger, "rt", "RT", "Accelerate", "Accelera", 'W'},
    {PadInput::kLeftTrigger, "lt", "LT", "Brake / reverse", "Frena / retromarcia", 'S'},
    {PadInput::kSteerLeft, "steer_left", "L \xE2\x86\x90", "Steer left", "Sterza a sinistra", 'A'},
    {PadInput::kSteerRight, "steer_right", "L \xE2\x86\x92", "Steer right", "Sterza a destra", 'D'},
    {PadInput::kA, "a", "A", "Handbrake", "Freno a mano", VK_SPACE},
    {PadInput::kB, "b", "B", "Weight transfer", "Trasferimento peso", VK_ESCAPE},
    {PadInput::kX, "x", "X", "Nitrous", "Nitro", 'Z'},
    {PadInput::kY, "y", "Y", "Look back", "Guarda indietro", 'C'},
    {PadInput::kLeftThumb, "ls", "LS", "Special ability", "Abilit\xC3\xA0 speciale", 'F'},
    {PadInput::kBlack, "black", "Black", "Change camera", "Cambia telecamera", VK_LSHIFT},
    {PadInput::kWhite, "white", "White", "Flash headlights", "Lampeggia i fari", VK_LCONTROL},
    {PadInput::kStart, "start", "Start", "Pause", "Pausa", VK_RETURN},
    {PadInput::kBack, "back", "Back", "Navigation system", "Navigatore", VK_BACK},
    {PadInput::kDpadUp, "up", "D-pad \xE2\x86\x91", "Map", "Mappa", VK_UP},
    {PadInput::kDpadDown, "down", "D-pad \xE2\x86\x93", "Hydraulics", "Idrauliche", VK_DOWN},
    {PadInput::kDpadLeft, "left", "D-pad \xE2\x86\x90", "Previous track", "Brano precedente", VK_LEFT},
    {PadInput::kDpadRight, "right", "D-pad \xE2\x86\x92", "Next track", "Brano successivo", VK_RIGHT},
    {PadInput::kLookLeft, "look_left", "R \xE2\x86\x90", "Camera left", "Telecamera sinistra", 'J'},
    {PadInput::kLookRight, "look_right", "R \xE2\x86\x92", "Camera right", "Telecamera destra", 'L'},
    {PadInput::kLookUp, "look_up", "R \xE2\x86\x91", "Camera up", "Telecamera su", 'I'},
    {PadInput::kLookDown, "look_down", "R \xE2\x86\x93", "Camera down", "Telecamera gi\xC3\xB9", 'K'},
};

std::atomic<int> g_keys[kPadInputs];  // read by the input thread

void save() {
    std::string text;
    for (const KeyBinding& b : kBindings) {
        char item[48];
        std::snprintf(item, sizeof(item), "%s%s:%d", text.empty() ? "" : " ", b.id, g_keys[static_cast<int>(b.input)].load());
        text += item;
    }
    setting_save_text("keyboard", text);
}

}  // namespace

const KeyBinding& key_binding(PadInput input) { return kBindings[static_cast<int>(input)]; }

int bound_key(PadInput input) { return g_keys[static_cast<int>(input)].load(); }

void bind_key(PadInput input, int virtual_key) {
    // A key does one thing: whatever had it before gets the key this input had.
    const int previous = bound_key(input);
    for (std::atomic<int>& key : g_keys) {
        if (key.load() == virtual_key) key = previous;
    }
    g_keys[static_cast<int>(input)] = virtual_key;
    save();
}

void reset_key_bindings() {
    for (const KeyBinding& b : kBindings) g_keys[static_cast<int>(b.input)] = b.default_key;
    save();
}

void load_key_bindings() {
    for (const KeyBinding& b : kBindings) g_keys[static_cast<int>(b.input)] = b.default_key;
    std::istringstream items(setting_text("keyboard"));
    for (std::string item; items >> item;) {
        const size_t colon = item.find(':');
        if (colon == std::string::npos) continue;
        const std::string id = item.substr(0, colon);
        const int key = std::atoi(item.c_str() + colon + 1);
        for (const KeyBinding& b : kBindings) {
            if (id == b.id && key > 0 && key < 256) g_keys[static_cast<int>(b.input)] = key;
        }
    }
}

}  // namespace mc3::pc
