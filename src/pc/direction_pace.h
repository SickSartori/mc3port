#pragma once

#include <cstdint>

namespace mc3::pc {

constexpr uint16_t kDpadUp = 0x01, kDpadDown = 0x02, kDpadLeft = 0x04, kDpadRight = 0x08;

struct DirectionPace {
    int direction = -1;
    uint64_t pulse_until = 0, next_pulse = 0;
};

// The garage's menus move on every poll a direction is down for long enough: at 60 fps a tap could skip an item. In
// there a press is one short pulse on the d-pad, and holding repeats at a menu's pace. Only there: elsewhere the stick
// steers, and nothing says for sure that the car is not being driven.
inline void pace_directions(DirectionPace& pace, uint64_t now_ms, uint16_t& buttons, int16_t& thumb_lx, int16_t& thumb_ly) {
    constexpr uint16_t kDirections[] = {kDpadUp, kDpadDown, kDpadLeft, kDpadRight};
    constexpr uint64_t kPulseMs = 50, kDelayMs = 400, kRateMs = 150;
    constexpr int kStickDown = 16000;
    int direction = -1;
    for (int i = 0; i < 4 && direction < 0; ++i) {
        if (buttons & kDirections[i]) direction = i;
    }
    if (direction < 0) {
        direction = thumb_ly > kStickDown    ? 0
                    : thumb_ly < -kStickDown ? 1
                    : thumb_lx < -kStickDown ? 2
                    : thumb_lx > kStickDown  ? 3
                                             : -1;
    }
    buttons &= ~(kDpadUp | kDpadDown | kDpadLeft | kDpadRight);
    thumb_lx = thumb_ly = 0;
    if (direction != pace.direction) {
        pace.direction = direction;
        pace.pulse_until = now_ms + kPulseMs;
        pace.next_pulse = now_ms + kDelayMs;
    } else if (direction >= 0 && now_ms >= pace.next_pulse) {
        pace.pulse_until = now_ms + kPulseMs;
        pace.next_pulse = now_ms + kRateMs;
    }
    if (direction >= 0 && now_ms < pace.pulse_until) buttons |= kDirections[direction];
}

}  // namespace mc3::pc
