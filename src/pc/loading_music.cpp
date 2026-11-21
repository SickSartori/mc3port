#include "pc/loading_music.h"
#include "pc/paths.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <memory>
#include <random>
#include <array>
#include <vector>

#include "pc/log.h"
#include "pc/settings.h"

namespace xport::pc {
namespace {
struct Track {
    int number = 0;
    uint32_t rate = 0;
    uint16_t channels = 0;
    std::vector<int16_t> samples;
    uint64_t frames() const { return channels ? samples.size() / channels : 0; }
};

std::mutex g_mutex;
size_t g_next = 0;
std::mt19937 g_rng{std::random_device{}()};
int g_last = -1, g_streak = 0;
std::array<int, 18> g_cooldown{};
std::shared_ptr<const Track> g_active;
uint64_t g_generation = 0;
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
    const size_t file_size = static_cast<size_t>(length);
    uint8_t header[12];
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(header), sizeof(header))) return false;
    if (std::memcmp(header, "RIFF", 4) || std::memcmp(header + 8, "WAVE", 4)) return false;
    size_t pos = 12;
    bool format_ok = false;
    size_t data_pos = 0, data_size = 0;
    while (pos + 8 <= file_size) {
        uint8_t chunk[8];
        in.seekg(static_cast<std::streamoff>(pos));
        if (!in.read(reinterpret_cast<char*>(chunk), sizeof(chunk))) return false;
        const uint32_t size = u32(chunk + 4);
        const size_t body = pos + 8;
        if (size > file_size - body) return false;
        if (!std::memcmp(chunk, "fmt ", 4) && size >= 16) {
            uint8_t fmt[16];
            if (!in.read(reinterpret_cast<char*>(fmt), sizeof(fmt))) return false;
            track.channels = u16(fmt + 2);
            track.rate = u32(fmt + 4);
            format_ok = u16(fmt) == 1 && (track.channels == 1 || track.channels == 2) &&
                        track.rate >= 8000 && track.rate <= 192000 && u16(fmt + 14) == 16 &&
                        u16(fmt + 12) == track.channels * 2;
        } else if (!std::memcmp(chunk, "data", 4)) {
            data_pos = body;
            data_size = size;
        }
        pos = body + size + (size & 1);
    }
    if (!format_ok || !data_pos || data_size < track.channels * 2 || data_size % (track.channels * 2)) return false;
    track.samples.resize(data_size / 2);
    in.seekg(static_cast<std::streamoff>(data_pos));
    // Windows x86 stores PCM16 samples in the WAV file's little-endian byte order.
    if (!in.read(reinterpret_cast<char*>(track.samples.data()), static_cast<std::streamsize>(data_size))) return false;
    return true;
}

// Probe only filenames here. WAV decoding happens after a track is selected.
std::vector<int> available_tracks() {
    std::vector<int> tracks;
    const auto folder = data_directory() / "loading_music";
    for (int number = 1; number <= 18; ++number) {
        std::error_code error;
        if (std::filesystem::is_regular_file(folder / (std::to_wstring(number) + L".wav"), error))
            tracks.push_back(number);
    }
    return tracks;
}
}  // namespace

void loading_music_begin(uint64_t duration_ms) {
    if (setting("loading_music") == 0 || setting("loading_music_volume") <= 0) return;

    // Directory probing and WAV decoding must never hold the audio mix mutex.
    const auto tracks = available_tracks();
    if (tracks.empty()) return;
    const int mode = std::clamp(static_cast<int>(setting("loading_music_mode")), 0, 2);
    std::array<double, 18> configured_weights{};
    if (mode == 2) {
        for (int number : tracks) {
            const std::string key = "loading_music_weight_" + std::to_string(number);
            configured_weights[number - 1] = std::clamp<double>(setting(key.c_str()), 0, 300);
        }
    }

    int number = 0;
    uint64_t generation = 0;
    {
        std::lock_guard lock(g_mutex);
        size_t selected = g_next++ % tracks.size();
        if (mode != 0) {
            std::vector<size_t> eligible;
            std::vector<double> weights;
            for (size_t i = 0; i < tracks.size(); ++i) {
                const int candidate = tracks[i];
                const double weight = mode == 1 ? 100.0 : configured_weights[candidate - 1];
                if (weight <= 0 || g_cooldown[candidate - 1] > 0 ||
                    (candidate == g_last && g_streak >= 2)) continue;
                eligible.push_back(i);
                weights.push_back(weight);
            }
            // Relax cooldowns if they exclude every enabled track.
            if (eligible.empty()) {
                for (size_t i = 0; i < tracks.size(); ++i) {
                    const double weight = mode == 1 ? 100.0 : configured_weights[tracks[i] - 1];
                    if (weight > 0) { eligible.push_back(i); weights.push_back(weight); }
                }
            }
            if (eligible.empty()) return; // All tracks intentionally disabled.
            std::discrete_distribution<size_t> pick(weights.begin(), weights.end());
            selected = eligible[pick(g_rng)];
        }
        number = tracks[selected];
        // A later begin/end invalidates a load that is still in progress.
        generation = ++g_generation;
    }

    auto loaded = std::make_shared<Track>();
    const auto file = data_directory() / "loading_music" / (std::to_wstring(number) + L".wav");
    if (!read_wave(file, *loaded)) {
        log_line("loading music: cannot read PCM WAV track %d", number);
        return;
    }
    loaded->number = number;

    std::shared_ptr<const Track> previous;
    {
        std::lock_guard lock(g_mutex);
        if (generation != g_generation) return;
        for (int& cooldown : g_cooldown) if (cooldown > 0) --cooldown;
        if (number == g_last) ++g_streak;
        else { g_last = number; g_streak = 1; }
        if (mode != 0 && g_streak >= 2) g_cooldown[number - 1] = 3;
        previous = std::move(g_active);
        g_active = std::move(loaded);
        g_position = 0;
        g_started = GetTickCount64();
        // Follow the loading-screen transition and fade during its extra tail.
        constexpr uint64_t kTransitionTailMs = 1000;
        g_end = g_started + duration_ms + kTransitionTailMs;
    }
    // Release the previous WAV buffer outside the audio mutex.
}

void loading_music_mix(float* stereo, uint32_t frames, uint32_t output_rate) {
    std::unique_lock lock(g_mutex);
    if (!g_active || !output_rate) return;
    const uint64_t now = GetTickCount64();
    if (now >= g_end) {
        auto expired = std::move(g_active);
        lock.unlock();
        // Release the expired WAV buffer outside the audio mutex.
        return;
    }
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
