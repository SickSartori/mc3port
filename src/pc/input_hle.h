#pragma once

#include "core/xbe.h"

namespace mc3::pc {

// Redirects the XPP gamepad functions (XGetDevices, XGetDeviceChanges, XInputOpen/Close/GetState/SetState/
// GetCapabilities) to the PC's XInput controllers, with the keyboard as controller 1 when no pad is there.
// Only for the XBE whose addresses were analysed (MC3 PAL).
//
// MC3_INPUT_SCRIPT="<seconds>:<button>,<from>-<to>:<button>,...": taps (200 ms) or holds buttons of a virtual
// controller 1 at those times since start (for automated tests). Buttons: start back a b x y black white up down
// left right lt rt, and sl/sr for the left stick (steering).
void install_input_hle(const Xbe& xbe);

}  // namespace mc3::pc
