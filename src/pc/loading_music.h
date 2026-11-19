#pragma once

#include <cstdint>

namespace xport::pc {
// Optional PCM WAV loading-screen music. No audio assets are distributed with mc3port.
void loading_music_begin(uint64_t duration_ms);
void loading_music_end();
// Called from the existing 48 kHz stereo output mixer before the master gain/limiter.
void loading_music_mix(float* stereo, uint32_t frames, uint32_t output_rate);
}  // namespace xport::pc
