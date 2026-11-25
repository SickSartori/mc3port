#pragma once

#include <cstdint>
#include <string>

#include "core/xbe.h"

namespace xport::pc {

// Redirects the XPP gamepad functions (XGetDevices, XGetDeviceChanges, XInputOpen/Close/GetState/SetState/
// GetCapabilities) to the PC's XInput controllers, with the keyboard as controller 1 when no pad is there.
// Only for the XBE whose addresses were analysed (MC3 PAL).
//
// MC3_INPUT_SCRIPT="<seconds>:<button>,<from>-<to>:<button>,...": taps (200 ms) or holds buttons of a virtual
// controller 1 at those times since start (for automated tests). Buttons: start back a b x y black white up down
// left right lt rt, and sl/sr for the left stick (steering).
void install_input_hle(const Xbe& xbe);

// For "@<file>" entries of MC3_INPUT_SCRIPT (times relative to the game opening a file).
void input_note_file_opened(const std::string& path);

// Watches the controllers' Back+Start (the in-game menu) from every frame the port shows, so a tap counts whatever
// the game's own polling pace.
void input_menu_combo_poll();

// The keyboard's keys and the mouse's buttons as the game window's messages report them (called first by its window
// procedure): controller 1 on the keyboard reads them here instead of asking Windows for the whole keyboard's state.
// Everything is let go when the window loses the focus.
void input_note_window_message(void* window, unsigned message, uintptr_t wparam, intptr_t lparam);
bool key_down(int virtual_key);
// The key of a WM_KEYDOWN/WM_KEYUP, with the left and right Shift, Ctrl and Alt told apart.
int key_from_message(uintptr_t wparam, intptr_t lparam);

}  // namespace xport::pc
