#pragma once

// The NV2A 3D class (Kelvin, NV097): state set by the push buffer and the draws it produces.

#include <cstdint>

namespace xport::pc {

using DmaResolver = uint32_t (*)(uint32_t handle);

void kelvin_initialise(DmaResolver resolver);
// F9: one second of MC3_DEBUG_FRAME_AT's draw lines from now (any thread).
void kelvin_request_debug_window();

// Executes one Kelvin method; false if the method is not handled (the caller logs it).
bool kelvin_method(uint32_t method, uint32_t argument);
// Several arguments of INLINE_ARRAY or ARRAY_ELEMENT16 at once: the same as a kelvin_method for each.
void kelvin_inline_data(const uint32_t* words, uint32_t count);
void kelvin_array_elements16(const uint32_t* words, uint32_t count);

// How much of the linear surface at `address` (physical) the game draws into: the widest clip seen there, 0 if
// none. The framebuffer can be wider than the picture (widescreen: 720 pixels in a 768-pixel pitch).
uint32_t kelvin_drawn_width(uint32_t address);

}  // namespace xport::pc
