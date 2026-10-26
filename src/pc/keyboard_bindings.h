#pragma once

// The keyboard keys that stand in for controller 1's buttons and sticks, editable in the launcher and stored in
// mc3port.ini (keyboard = "a:32 b:27 ..." with Windows virtual-key codes).

#include <cstdint>

namespace mc3::pc {

enum class PadInput : uint8_t {
    kRightTrigger, kLeftTrigger, kSteerLeft, kSteerRight,
    kA, kB, kX, kY, kBlack, kWhite,
    kStart, kBack, kDpadUp, kDpadDown, kDpadLeft, kDpadRight,
    kLookLeft, kLookRight, kLookUp, kLookDown,
    kCount
};
constexpr int kPadInputs = static_cast<int>(PadInput::kCount);

struct KeyBinding {
    PadInput input;
    const char* id;      // in mc3port.ini
    const char* button;  // the controller part, as printed on the pad
    const char* english;
    const char* italian;
    int default_key;
};

const KeyBinding& key_binding(PadInput input);
int bound_key(PadInput input);                // its Windows virtual-key code
void bind_key(PadInput input, int virtual_key);  // and saves all bindings
void reset_key_bindings();
void load_key_bindings();  // from the settings (after settings_load)

}  // namespace mc3::pc
