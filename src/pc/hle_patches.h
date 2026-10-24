#pragma once

#include "core/xbe.h"

namespace mc3::pc {

// Replaces XDK library functions that drive hardware the port does not emulate (the USB host controller behind
// the controllers and memory units) with host implementations. Addresses are specific to the PAL disc's XBE;
// every patch checks the original bytes first and is skipped, with a log line, if they differ.
void apply_hle_patches(const Xbe& xbe);

}  // namespace mc3::pc
