// The fixed frame step: whole sixtieths, changed only when the frames clearly change pace.

#include "pc/frame_step.h"
#include "test.h"

using namespace xport::pc;

int main() {
    CHECK(step_sixtieths(0, 1.2f) == 1);   // first measurement: the nearest, at least one
    CHECK(step_sixtieths(0, 0.4f) == 1);
    CHECK(step_sixtieths(0, 2.2f) == 2);
    CHECK(step_sixtieths(1, 1.6f) == 1);   // a few late frames: still 1/60 s (rounded, this was 1/30)
    CHECK(step_sixtieths(1, 1.74f) == 1);
    CHECK(step_sixtieths(1, 1.8f) == 2);   // clearly slower: 1/30 s
    CHECK(step_sixtieths(2, 1.45f) == 2);  // just back under 1.5: stays
    CHECK(step_sixtieths(2, 1.35f) == 1);  // clearly back to 60 fps
    CHECK(step_sixtieths(2, 3.0f) == 3);   // much slower: straight to 1/20 s
    CHECK(step_sixtieths(3, 1.0f) == 1);   // back to full speed in one go
    return test_result();
}
