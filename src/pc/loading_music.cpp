#include "pc/loading_music.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include "pc/log.h"
#include "pc/settings.h"

namespace xport::pc {
namespace {
struct Track {
    uint32_t rate = 0;
    uint16_t channels = 0;
    std::vector<int16_t> samples;
    uint64_t frames() const { return channels ? samples.size() / channels : 0; }
};

std::mutex g_mutex;
std::vector<Track> g_tracks;
bool g_loaded = false;
size_t g_next = 0;
const Track* g_active = nullptr;
double g_position = 0;
uint64_t g_started = 0;
uint64_t g_end = 0;

uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | (uint16_t(p[1]) << 8)); }
uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

// Read standard RIFF/WAVE PCM16 mono/stereo; skip optional metadata chunks.
bool read_wave(const std::filesystem::path& file, Track& track) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const auto length = in.tellg();
    if (length < 44 || length > 32 * 1024 * 1024) return false;
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return false;
    if (std::memcmp(bytes.data(), "RIFF", 4) || std::memcmp(bytes.data() + 8, "WAVE", 4)) return false;
    size_t pos = 12;
    bool format_ok = false;
    size_t data_pos = 0, data_size = 0;
    while (pos + 8 <= bytes.size()) {
        const uint32_t size = u32(bytes.data() + pos + 4);
        const size_t body = pos + 8;
        if (size > bytes.size() - body) return false;
        if (!std::memcmp(bytes.data() + pos, "fmt ", 4) && size >= 16) {
            const uint8_t* fmt = bytes.data() + body;
            track.channels = u16(fmt + 2);
            track.rate = u32(fmt + 4);
            format_ok = u16(fmt) == 1 && (track.channels == 1 || track.channels == 2) &&
                        track.rate >= 8000 && track.rate <= 192000 && u16(fmt + 14) == 16 &&
                        u16(fmt + 12) == track.channels * 2;
        } else if (!std::memcmp(bytes.data() + pos, "data", 4)) {
            data_pos = body;
            data_size = size;
        }
        pos = body + size + (size & 1);
    }
    if (!format_ok || !data_pos || data_size < track.channels * 2 || data_size % (track.channels * 2)) return false;
    track.samples.resize(data_size / 2);
    for (size_t i = 0; i < track.samples.size(); ++i) track.samples[i] = static_cast<int16_t>(u16(bytes.data() + data_pos + i * 2));
    return true;
}

void load_tracks() {
    if (g_loaded) return;
    g_loaded = true;
    // Beside the executable, independent of the user's game-data/portable-mode location.
    wchar_t module[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, module, MAX_PATH)) return;
    const auto folder = std::filesystem::path(module).parent_path() / L"loading_music";
    for (int i = 1; i <= 18; ++i) {
        Track track;
        if (read_wave(folder / (std::to_wstring(i) + L".wav"), track)) g_tracks.push_back(std::move(track));
    }
    log_line("loading music: %u PCM WAV tracks found", static_cast<unsigned>(g_tracks.size()));
}
}  // namespace

void loading_music_begin(uint64_t duration_ms) {
    if (setting("loading_music") == 0 || setting("loading_music_volume") <= 0) return;
    std::lock_guard lock(g_mutex);
    load_tracks();
    if (g_tracks.empty()) return;
    g_active = &g_tracks[g_next++ % g_tracks.size()];
    g_position = 0;
    g_started = GetTickCount64();
    // Allow the audio to follow the visible loading-screen transition, which can
    // outlast the minimum loading timer. Fade during this extra tail.
    constexpr uint64_t kTransitionTailMs = 1000;
    g_end = g_started + duration_ms + kTransitionTailMs;
}

void loading_music_end() {
    std::lock_guard lock(g_mutex);
    g_active = nullptr;
}

void loading_music_mix(float* stereo, uint32_t frames, uint32_t output_rate) {
    std::lock_guard lock(g_mutex);
    if (!g_active || !output_rate) return;
    const uint64_t now = GetTickCount64();
    if (now >= g_end) { g_active = nullptr; return; }
    const auto& track = *g_active;
    const uint64_t count = track.frames();
    if (!count) return;
    const float volume = std::clamp(setting("loading_music_volume"), 0.0f, 100.0f) / 100.0f;
    const double fade_ms = std::clamp<double>(setting("loading_music_fade_seconds") * 1000.0, 0.0, 10000.0);
    const double step = static_cast<double>(track.rate) / output_rate;
    for (uint32_t i = 0; i < frames; ++i) {
        const double elapsed_ms = static_cast<double>(i) * 1000.0 / output_rate;
        const double remaining_ms = static_cast<double>(g_end - now) - elapsed_ms;
        if (remaining_ms <= 0) break;
        const float fade = fade_ms > 0 ? static_cast<float>(std::clamp(remaining_ms / fade_ms, 0.0, 1.0)) : 1.0f;
        const uint64_t a = static_cast<uint64_t>(g_position) % count;
        const uint64_t b = (a + 1) % count;
        const float fraction = static_cast<float>(g_position - std::floor(g_position));
        for (uint16_t channel = 0; channel < 2; ++channel) {
            const uint16_t source_channel = track.channels == 1 ? 0 : channel;
            const float left = track.samples[a * track.channels + source_channel] / 32768.0f;
            const float right = track.samples[b * track.channels + source_channel] / 32768.0f;
            stereo[i * 2 + channel] += (left + (right - left) * fraction) * volume * fade;
        }
        g_position += step;
        if (g_position >= count) g_position = std::fmod(g_position, static_cast<double>(count));
    }
}
}  // namespace xport::pc
