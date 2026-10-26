#pragma once

// The NV2A 3D class (Kelvin, NV097): state set by the push buffer and the draws it produces.

#include <cstdint>

namespace mc3::pc {

using DmaResolver = uint32_t (*)(uint32_t handle);

void kelvin_initialise(DmaResolver resolver);

// Executes one Kelvin method; false if the method is not handled (the caller logs it).
bool kelvin_method(uint32_t method, uint32_t argument);

// How much of the linear surface at `address` (physical) the game draws into: the widest clip seen there, 0 if
// none. The framebuffer can be wider than the picture (widescreen: 720 pixels in a 768-pixel pitch).
uint32_t kelvin_drawn_width(uint32_t address);

}  // namespace mc3::pc
