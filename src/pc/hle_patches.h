#pragma once

#include <cstdint>

#include "core/xbe.h"

namespace mc3::pc {

// Replaces XDK library functions that drive hardware the port does not emulate (the USB host controller behind
// the controllers and memory units) with host implementations. Addresses are specific to the PAL disc's XBE;
// every patch checks the original bytes first and is skipped, with a log line, if they differ.
void apply_hle_patches(const Xbe& xbe);

// The display showed a new game frame, `vblanks` vertical blanks after the previous one: the game's fixed time step
// follows how long its frames really stay on screen (see hle_patches.cpp, frame step).
void time_frame_shown(uint32_t vblanks);

}  // namespace mc3::pc
