// The garage's menus: a press is one step, holding repeats at a menu's pace, at any frame rate.

#include <cstdint>

#include "pc/direction_pace.h"
#include "test.h"

using namespace xport::pc;

namespace {

constexpr uint16_t kAllDirections = kDpadUp | kDpadDown | kDpadLeft | kDpadRight;
constexpr uint16_t kA = 0x10;

// Steps the game sees (each rise of a direction) during `ms`, polling every `poll` ms from `now` on.
int steps(DirectionPace& pace, uint64_t& now, uint64_t ms, uint64_t poll, uint16_t buttons, int16_t lx = 0, int16_t ly = 0,
          uint16_t seen = kDpadDown) {
    int count = 0;
    bool was_down = false;
    for (const uint64_t end = now + ms; now < end; now += poll) {
        uint16_t b = buttons;
        int16_t x = lx, y = ly;
        pace_directions(pace, now, b, x, y);
        CHECK(x == 0 && y == 0);  // the stick never reaches the menu itself
        const bool down = (b & seen) != 0;
        if (down && !was_down) ++count;
        was_down = down;
    }
    return count;
}

}  // namespace

int main() {
    for (const uint64_t poll : {4ull, 16ull, 33ull}) {  // 240, 60, 30 polls per second
        DirectionPace pace;
        uint64_t now = 1000;
        CHECK(steps(pace, now, 100, poll, 0) == 0);

        // A tap of any length up to the delay is one step: the bug was skipping items.
        for (const uint64_t tap : {60ull, 200ull, 380ull}) {
            CHECK(steps(pace, now, tap, poll, kDpadDown) == 1);
            CHECK(steps(pace, now, 300, poll, 0) == 0);
        }

        // Holding: at once, after 400 ms, then every 150 ms; 1 s holds 5 steps (0, 400, 550, 700, 850), give or take a poll.
        const int held = steps(pace, now, 1000, poll, kDpadDown);
        CHECK(held >= 5 && held <= 6);
        CHECK(steps(pace, now, 300, poll, 0) == 0);

        // Another direction moves at once, with its own delay.
        CHECK(steps(pace, now, 100, poll, kDpadDown) == 1);
        CHECK(steps(pace, now, 100, poll, kDpadUp, 0, 0, kDpadUp) == 1);
        CHECK(steps(pace, now, 300, poll, 0) == 0);
    }

    DirectionPace pace;
    uint64_t now = 1000;
    // The stick: past the threshold it is the d-pad, under it nothing.
    CHECK(steps(pace, now, 100, 16, 0, 0, -16001) == 1);
    CHECK(steps(pace, now, 300, 16, 0) == 0);
    CHECK(steps(pace, now, 100, 16, 0, 0, -15999) == 0);
    CHECK(steps(pace, now, 100, 16, 0, 0, 16001, kDpadUp) == 1);
    CHECK(steps(pace, now, 300, 16, 0) == 0);
    CHECK(steps(pace, now, 100, 16, 0, -16001, 0, kDpadLeft) == 1);
    CHECK(steps(pace, now, 300, 16, 0) == 0);
    CHECK(steps(pace, now, 100, 16, 0, 16001, 0, kDpadRight) == 1);
    CHECK(steps(pace, now, 300, 16, 0) == 0);

    // The d-pad wins over the stick, and only one direction goes through.
    uint16_t buttons = kDpadLeft | kDpadRight;
    int16_t x = 0, y = 20000;
    pace_directions(pace, now, buttons, x, y);
    CHECK((buttons & kAllDirections) == kDpadLeft);

    // The other buttons are not touched.
    DirectionPace other;
    buttons = kA;
    x = y = 0;
    pace_directions(other, 5000, buttons, x, y);
    CHECK(buttons == kA);
    buttons = kA | kDpadUp;
    pace_directions(other, 5016, buttons, x, y);
    CHECK(buttons == (kA | kDpadUp));
    buttons = kA | kDpadUp;
    pace_directions(other, 5100, buttons, x, y);  // the pulse is over
    CHECK(buttons == kA);
    return test_result();
}
