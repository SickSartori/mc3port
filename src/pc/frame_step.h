#pragma once

#include <algorithm>
#include <cmath>

namespace xport::pc {

// The fixed step in whole sixtieths of a second, from the one in use and the measured average frame time (in
// sixtieths). It moves only when the frames clearly changed pace: up past 3/4 of a sixtieth above, down past 0.6
// below. Rounded at the half, a few late frames (a burst of new textures entering the city) lifted the average past
// 1.5 and the game took one step of 1/30 s, then went back to 1/60: a jump in the picture, the micro stutter of the
// 0.3.6 reports, thousands of times an hour on a slow PC.
inline float step_sixtieths(float current, float measured) {
    const float nearest = std::max(1.0f, std::round(measured));
    if (current < 1) return nearest;  // first measurement
    if (nearest > current && measured < current + 0.75f) return current;
    if (nearest < current && measured > current - 0.6f) return current;
    return nearest;
}

}  // namespace xport::pc
