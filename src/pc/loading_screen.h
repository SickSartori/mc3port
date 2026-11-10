#pragma once

// Midnight Club 3's loading screens (setting loading_screen_seconds), between kernel_io.cpp and kernel_threads.cpp.

namespace xport::pc {

// A loading screen opened (its Flash file): the loading behind it lasts at least loading_screen_seconds.
void note_loading_screen();
// On the thread that opened a loading screen, the next file it opens waits until the screen has been up long enough.
void hold_loading_screen();

}  // namespace xport::pc
