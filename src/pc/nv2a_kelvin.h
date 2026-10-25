#pragma once

// The NV2A 3D class (Kelvin, NV097): state set by the push buffer and the draws it produces.

#include <cstdint>

namespace mc3::pc {

using DmaResolver = uint32_t (*)(uint32_t handle);

void kelvin_initialise(DmaResolver resolver);

// Executes one Kelvin method; false if the method is not handled (the caller logs it).
bool kelvin_method(uint32_t method, uint32_t argument);

}  // namespace mc3::pc
