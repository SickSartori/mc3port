#include "pc/audio_mixer.h"

#include <windows.h>
#include <xaudio2.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "pc/handles.h"
#include "pc/kernel_internal.h"
#include "pc/log.h"
#include "pc/xbox_types.h"

namespace mc3::pc {
namespace {

constexpr uint32_t kFramesPerChunk = kOutputRate / 100;  // 10 ms
constexpr uint32_t kQueuedChunks = 4;
constexpr uint32_t kChunkRing = 8;
constexpr uint16_t kFormatPcm = 1;
constexpr uint16_t kFormatXboxAdpcm = 0x69;
constexpr uint32_t kAdpcmBlockBytes = 36;  // per channel
constexpr uint32_t kAdpcmBlockFrames = 64;
constexpr int32_t kSilence = -10000;
constexpr uint32_t kPacketFlushed = 0x80004004;

std::mutex g_mutex;
std::vector<Voice*> g_voices;
std::vector<CompletedPacket> g_completed;

// Activity since the last report, to confirm in the log that sound actually reaches the device.
struct MixStatistics {
    uint32_t chunks = 0;
    uint32_t audible_chunks = 0;
    float peak = 0;
    uint32_t most_voices = 0;
    uint32_t completed_packets = 0;
    uint32_t limited_samples = 0;  // softened by the limiter
};
MixStatistics g_statistics;

// --- Xbox ADPCM (IMA ADPCM, 36-byte blocks per channel, 64 samples per block) ---

constexpr int16_t kStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
constexpr int8_t kIndexTable[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

// Decodes one block into 64 interleaved frames. The header sample is the first one; the data's 64 nibbles give
// 64 more of which the first 63 are kept, so blocks stay 64 frames long as the format declares.
void decode_adpcm_block(const uint8_t* block, uint32_t channels, int16_t* out) {
    for (uint32_t channel = 0; channel < channels; ++channel) {
        const uint8_t* header = block + channel * 4;
        int32_t predictor = static_cast<int16_t>(header[0] | (header[1] << 8));
        int32_t index = std::clamp<int32_t>(header[2], 0, 88);
        out[channel] = static_cast<int16_t>(predictor);
        uint32_t frame = 1;
        // After the headers, data is interleaved in 4-byte words per channel (8 nibbles each).
        for (uint32_t word = 0; word < 8 && frame < kAdpcmBlockFrames; ++word) {
            const uint8_t* bytes = block + channels * 4 + (word * channels + channel) * 4;
            for (uint32_t nibble_index = 0; nibble_index < 8 && frame < kAdpcmBlockFrames; ++nibble_index) {
                const uint8_t nibble = (bytes[nibble_index / 2] >> ((nibble_index & 1) * 4)) & 0xF;
                const int32_t step = kStepTable[index];
                int32_t delta = step >> 3;
                if (nibble & 1) delta += step >> 2;
                if (nibble & 2) delta += step >> 1;
                if (nibble & 4) delta += step;
                predictor = std::clamp<int32_t>(nibble & 8 ? predictor - delta : predictor + delta, -32768, 32767);
                index = std::clamp<int32_t>(index + kIndexTable[nibble & 7], 0, 88);
                out[frame * channels + channel] = static_cast<int16_t>(predictor);
                ++frame;
            }
        }
    }
}

// --- sample access ---

uint32_t bytes_per_frame(const XWaveFormat& format) {
    return std::max<uint32_t>(1, format.channels * std::max<uint32_t>(8, format.bits_per_sample) / 8);
}

// Sample (as float) of `channel` at `frame` bytes-relative to `base` within [data, data + size).
float pcm_sample(const uint8_t* data, uint32_t size, const XWaveFormat& format, uint32_t frame, uint32_t channel) {
    const uint32_t offset = frame * bytes_per_frame(format) + channel * (format.bits_per_sample / 8);
    if (format.bits_per_sample == 8) {
        if (offset >= size) return 0;
        return (static_cast<int32_t>(data[offset]) - 128) / 128.0f;
    }
    if (offset + 1 >= size) return 0;
    return static_cast<int16_t>(data[offset] | (data[offset + 1] << 8)) / 32768.0f;
}

float voice_sample(Voice& voice, const uint8_t* data, uint32_t size, uint32_t frame, uint32_t channel) {
    if (voice.format.format_tag != kFormatXboxAdpcm) return pcm_sample(data, size, voice.format, frame, channel);
    const uint32_t channels = std::max<uint32_t>(1, voice.format.channels);
    const uint32_t block = frame / kAdpcmBlockFrames;
    const uint32_t block_bytes = kAdpcmBlockBytes * channels;
    if ((block + 1) * block_bytes > size) return 0;
    // The cache is keyed by the absolute block address, so it also works across stream packets.
    const int64_t key = static_cast<int64_t>(reinterpret_cast<uintptr_t>(data + block * block_bytes));
    if (voice.decoded_block != key) {
        voice.decoded.resize(kAdpcmBlockFrames * channels);
        decode_adpcm_block(data + block * block_bytes, channels, voice.decoded.data());
        voice.decoded_block = key;
    }
    return voice.decoded[(frame % kAdpcmBlockFrames) * channels + channel] / 32768.0f;
}

float decibels_to_gain(int32_t hundredths) {
    if (hundredths <= kSilence) return 0;
    return std::pow(10.0f, static_cast<float>(hundredths) / 2000.0f);
}

// Left/right gain of each source channel, from the voice volume and its mix bins: the 5.1 speaker bins folded to
// stereo (front 1, centre and back 0.5 per side; LFE and effect sends, whose effects are not emulated, left out).
// Bink and XACT send each channel to all six speaker bins at full level; the plain sum was 2.7 times too loud.
void channel_gains(const Voice& voice, float gains[2][2]) {
    std::memset(gains, 0, sizeof(float) * 4);
    const uint32_t channels = std::clamp<uint32_t>(voice.format.channels, 1, 2);
    if (voice.mix_bin_count == 0) {
        const float gain = decibels_to_gain(voice.volume);
        if (channels == 1) {
            gains[0][0] = gains[0][1] = gain;
        } else {
            gains[0][0] = gains[1][1] = gain;
        }
        return;
    }
    for (int32_t i = 0; i < voice.mix_bin_count; ++i) {
        const uint32_t bin = voice.mix_bins[i];
        const uint32_t channel = static_cast<uint32_t>(i) % channels;
        const float gain = decibels_to_gain(voice.volume + (bin < 32 ? voice.mix_bin_volumes[bin] : 0));
        switch (bin) {
            case 0: gains[channel][0] += gain; break;                                      // front left
            case 1: gains[channel][1] += gain; break;                                      // front right
            case 2: gains[channel][0] += gain * 0.5f; gains[channel][1] += gain * 0.5f; break;  // centre
            case 4: gains[channel][0] += gain * 0.5f; break;                               // back left
            case 5: gains[channel][1] += gain * 0.5f; break;                               // back right
            default: break;  // 3 LFE, 6+ effect sends
        }
    }
}

double step_for(const Voice& voice) { return std::pow(2.0, voice.pitch / 4096.0); }

// One output frame of the voice's DLS2 low-pass (Chamberlin state-variable filter) on one source channel.
float filtered(Voice& voice, uint32_t channel, float sample) {
    VoiceFilter& filter = voice.filter;
    if (!filter.enabled) return sample;
    float& low = filter.low[channel];
    float& band = filter.band[channel];
    low += filter.frequency[channel] * band;
    const float high = sample - low - filter.damping[channel] * band;
    band += filter.frequency[channel] * high;
    if (std::abs(low) < 1e-20f) low = 0;  // no denormals once the sound has died away
    if (std::abs(band) < 1e-20f) band = 0;
    return low;
}

// --- amplitude envelope ---

constexpr uint32_t kShortestRelease = 240;  // 5 ms: a release of 0 still must not click

uint32_t stage_frames(const Voice& voice, EnvelopeStage stage) {
    const Envelope& envelope = voice.envelope;
    switch (stage) {
        case EnvelopeStage::kDelay: return envelope.delay;
        case EnvelopeStage::kAttack: return envelope.attack;
        case EnvelopeStage::kHold: return envelope.hold;
        case EnvelopeStage::kDecay: return envelope.decay;
        case EnvelopeStage::kRelease: return std::max(envelope.release, kShortestRelease);
        default: return 0;
    }
}

// Moves the envelope on by one chunk and returns its level at the end of the chunk.
float advance_envelope(Voice& voice, uint32_t frames) {
    EnvelopeStage& stage = voice.envelope_stage;
    if (stage == EnvelopeStage::kOff) return 1;
    voice.envelope_frames += frames;
    while (stage != EnvelopeStage::kSustain && stage != EnvelopeStage::kDone) {
        const uint32_t length = stage_frames(voice, stage);
        if (voice.envelope_frames < length) break;
        voice.envelope_frames -= length;
        stage = static_cast<EnvelopeStage>(static_cast<uint8_t>(stage) + 1);
        if (stage == EnvelopeStage::kRelease) stage = EnvelopeStage::kDone;  // sustain is left only by a release
    }
    const float sustain = voice.envelope.sustain;
    const float t = stage_frames(voice, stage)
                        ? static_cast<float>(voice.envelope_frames) / static_cast<float>(stage_frames(voice, stage))
                        : 1.0f;
    switch (stage) {
        case EnvelopeStage::kDelay: voice.envelope_level = 0; break;
        case EnvelopeStage::kAttack: voice.envelope_level = t; break;
        case EnvelopeStage::kHold: voice.envelope_level = 1; break;
        case EnvelopeStage::kDecay: voice.envelope_level = 1 + (sustain - 1) * t; break;
        case EnvelopeStage::kSustain: voice.envelope_level = sustain; break;
        case EnvelopeStage::kRelease: voice.envelope_level = voice.release_from * (1 - t); break;
        default: voice.envelope_level = 0; break;
    }
    return voice.envelope_level;
}

// The release has faded the voice out: stop it, as the APU does at the end of the envelope.
void finish_release(Voice& voice) {
    voice.envelope_stage = EnvelopeStage::kOff;
    voice.envelope_level = 1;
    voice.playing = false;
    if (!voice.is_stream) return;
    voice.end_of_stream = false;
    voice.stopped_by_flush = true;
    for (const StreamPacket& packet : voice.packets) complete_packet(voice, packet, kPacketFlushed);
    voice.packets.clear();
    voice.stream_fraction = 0;
    voice.decoded_block = -1;
}

// Per-chunk gain ramp: from the previous chunk's gains to the new ones.
struct GainRamp {
    float start[2][2], delta[2][2];
    GainRamp(Voice& voice, uint32_t frames) {
        float target[2][2];
        channel_gains(voice, target);
        const float level = advance_envelope(voice, frames);
        for (int c = 0; c < 2; ++c) target[c][0] *= level, target[c][1] *= level;
        if (!voice.has_last_gains) std::memcpy(voice.last_gains, target, sizeof(target));
        for (int c = 0; c < 2; ++c) {
            for (int s = 0; s < 2; ++s) {
                start[c][s] = voice.last_gains[c][s];
                delta[c][s] = (target[c][s] - start[c][s]) / static_cast<float>(frames);
            }
        }
        std::memcpy(voice.last_gains, target, sizeof(target));
        voice.has_last_gains = true;
    }
    float at(uint32_t channel, uint32_t side, uint32_t n) const { return start[channel][side] + delta[channel][side] * n; }
};

// --- mixing ---

void mix_buffer_voice(Voice& voice, float* out, uint32_t frames) {
    if (!voice.data || voice.size == 0) {
        voice.playing = false;
        return;
    }
    const uint32_t region_bytes = play_region_bytes(voice);
    const uint8_t* region = voice.data + voice.play_start;
    const uint32_t region_frames = bytes_to_frames(voice, region_bytes);
    if (region_frames == 0) {
        voice.playing = false;
        return;
    }
    uint32_t loop_start = std::min(bytes_to_frames(voice, voice.loop_start), region_frames - 1);
    uint32_t loop_frames = voice.loop_length ? bytes_to_frames(voice, voice.loop_length) : region_frames - loop_start;
    loop_frames = std::max<uint32_t>(1, std::min(loop_frames, region_frames - loop_start));
    const uint32_t loop_end = loop_start + loop_frames;

    const GainRamp gains(voice, frames);
    const uint32_t channels = std::clamp<uint32_t>(voice.format.channels, 1, 2);
    const double step = step_for(voice);

    for (uint32_t n = 0; n < frames; ++n) {
        // Linear interpolation between this frame and the next one (which wraps to the loop start): picking the
        // nearest frame aliases badly whenever the pitch is not exactly 1 (engines change it all the time).
        const uint32_t frame = static_cast<uint32_t>(voice.position);
        const float fraction = static_cast<float>(voice.position - frame);
        const uint32_t end = voice.looping ? loop_end : region_frames;
        const uint32_t next = frame + 1 < end ? frame + 1 : (voice.looping ? loop_start : frame);
        for (uint32_t channel = 0; channel < channels; ++channel) {
            const float a = voice_sample(voice, region, region_bytes, frame, channel);
            const float b = fraction > 0 ? voice_sample(voice, region, region_bytes, next, channel) : a;
            const float sample = filtered(voice, channel, a + (b - a) * fraction);
            out[n * 2] += sample * gains.at(channel, 0, n);
            out[n * 2 + 1] += sample * gains.at(channel, 1, n);
        }
        voice.position += step;
        if (voice.position >= end) {
            if (voice.looping) {
                voice.position = loop_start + std::fmod(voice.position - end, static_cast<double>(loop_frames));
            } else {
                voice.position = region_frames;
                voice.playing = false;
                return;
            }
        }
    }
}

void mix_stream_voice(Voice& voice, float* out, uint32_t frames) {
    const GainRamp gains(voice, frames);
    const uint32_t channels = std::clamp<uint32_t>(voice.format.channels, 1, 2);
    const double step = step_for(voice);

    for (uint32_t n = 0; n < frames && !voice.packets.empty(); ++n) {
        StreamPacket& current = voice.packets.front();
        const auto* data = guest<const uint8_t>(current.packet.buffer);
        const uint32_t packet_frames = bytes_to_frames(voice, current.packet.max_size);
        const uint32_t frame = current.consumed_frames;
        if (frame < packet_frames) {
            const float fraction = static_cast<float>(voice.stream_fraction);
            for (uint32_t channel = 0; channel < channels; ++channel) {
                const float a = voice_sample(voice, data, current.packet.max_size, frame, channel);
                // The next frame, possibly the first of the next packet.
                float b = a;
                if (fraction > 0) {
                    if (frame + 1 < packet_frames) {
                        b = voice_sample(voice, data, current.packet.max_size, frame + 1, channel);
                    } else if (voice.packets.size() > 1 && voice.format.format_tag != kFormatXboxAdpcm) {
                        const StreamPacket& following = voice.packets[1];
                        b = voice_sample(voice, guest<const uint8_t>(following.packet.buffer),
                                         following.packet.max_size, 0, channel);
                    }
                }
                const float sample = filtered(voice, channel, a + (b - a) * fraction);
                out[n * 2] += sample * gains.at(channel, 0, n);
                out[n * 2 + 1] += sample * gains.at(channel, 1, n);
            }
        }
        voice.stream_fraction += step;
        const uint32_t advanced = static_cast<uint32_t>(voice.stream_fraction);
        voice.stream_fraction -= advanced;
        current.consumed_frames += advanced;
        if (current.consumed_frames >= packet_frames) {
            complete_packet(voice, current, kStatusSuccess);
            voice.packets.pop_front();
            voice.decoded_block = -1;
        }
    }
    if (voice.packets.empty() && voice.end_of_stream) {
        voice.playing = false;
        voice.end_of_stream = false;
    }
}

void mix(float* out, uint32_t frames) {
    std::memset(out, 0, frames * 2 * sizeof(float));
    std::lock_guard<std::mutex> lock(g_mutex);
    uint32_t active = 0;
    for (Voice* voice : g_voices) {
        if (voice->paused) {
            // A paused voice is silent: a release asked for meanwhile ends at once. The game's music player stops a
            // still-paused stream and waits, holding a lock, for it to stop playing: a release that never advanced
            // froze the game at the title screen.
            if (voice->envelope_stage == EnvelopeStage::kRelease || voice->envelope_stage == EnvelopeStage::kDone) {
                finish_release(*voice);
            }
            continue;
        }
        if (voice->is_stream) {
            if (!voice->packets.empty()) ++active, ++voice->active_chunks;
            mix_stream_voice(*voice, out, frames);
        } else if (voice->playing) {
            ++active, ++voice->active_chunks;
            mix_buffer_voice(*voice, out, frames);
        }
        if (voice->envelope_stage == EnvelopeStage::kDone) finish_release(*voice);
    }
    // Master level (the Xbox keeps headroom too), then a soft limiter instead of hard clipping: linear up to 0.8,
    // a smooth knee above, so many voices at once round off instead of crackling.
    constexpr float kMasterGain = 0.4f, kKnee = 0.8f;  // 0.5 still kept the limiter busy while driving
    float peak = 0;
    uint32_t limited = 0;
    for (uint32_t i = 0; i < frames * 2; ++i) {
        float x = out[i] * kMasterGain;
        const float magnitude = std::abs(x);
        if (magnitude > kKnee) {
            x = std::copysign(kKnee + (1.0f - kKnee) * std::tanh((magnitude - kKnee) / (1.0f - kKnee)), x);
            ++limited;
        }
        out[i] = x;
        peak = std::max(peak, std::abs(x));
    }
    g_statistics.limited_samples += limited;
    MixStatistics& statistics = g_statistics;
    ++statistics.chunks;
    if (peak > 0.001f) ++statistics.audible_chunks;
    statistics.peak = std::max(statistics.peak, peak);
    statistics.most_voices = std::max(statistics.most_voices, active);
    if (statistics.chunks == 1000) {  // every 10 s
        log_line("audio: last 10 s: %u%% audible, peak %.2f, %u samples limited, up to %u voices, %u stream packets "
                 "completed, %zu voices",
                 statistics.audible_chunks / 10, statistics.peak, statistics.limited_samples, statistics.most_voices,
                 statistics.completed_packets, g_voices.size());
        statistics = MixStatistics{};
        // MC3_AUDIO_VOICES=1: which voices are playing and how loud (for overlaps).
        static const bool list_voices = GetEnvironmentVariableA("MC3_AUDIO_VOICES", nullptr, 0) != 0;
        if (list_voices) {
            for (const Voice* voice : g_voices) {
                const bool active = voice->is_stream ? !voice->packets.empty() : voice->playing;
                if (!active || voice->paused) continue;
                float gains[2][2];
                channel_gains(*voice, gains);
                std::string bins;
                for (int32_t i = 0; i < voice->mix_bin_count; ++i) {
                    char entry[24];
                    std::snprintf(entry, sizeof(entry), " %u:%d", voice->mix_bins[i],
                                  voice->mix_bin_volumes[voice->mix_bins[i] & 31]);
                    bins += entry;
                }
                log_line("  voice %p %s flags %08X fmt %X/%u Hz/%u bit ch %u vol %d pitch %d loop %d size %u region "
                         "%u+%u loop %u+%u pos %.0f age %.1f s gain L %.2f R %.2f bins%s",
                         static_cast<const void*>(voice), voice->is_stream ? "stream" : "buffer", voice->create_flags,
                         voice->format.format_tag, voice->format.samples_per_second, voice->format.bits_per_sample,
                         voice->format.channels, voice->volume, voice->pitch, voice->looping, voice->size,
                         voice->play_start, voice->play_length, voice->loop_start, voice->loop_length,
                         voice->position, voice->active_chunks / 100.0, gains[0][0] + gains[1][0],
                         gains[0][1] + gains[1][1], bins.c_str());
            }
        }
    }
}

// MC3_AUDIO_DUMP=<seconds>: the first <seconds> of mixer output are written to audio.wav (16-bit stereo), to check
// the sound without listening to it.
class WavDump {
public:
    WavDump() {
        char value[16] = {};
        if (!GetEnvironmentVariableA("MC3_AUDIO_DUMP", value, sizeof(value))) return;
        limit_ = static_cast<uint32_t>(std::atof(value) * kOutputRate);
        file_ = std::fopen("audio.wav", "wb");
        if (file_) write_header();
    }
    void add(const float* samples, uint32_t frames) {
        if (!file_) return;
        frames = std::min(frames, limit_ - written_);
        std::vector<int16_t> pcm(frames * 2);
        for (uint32_t i = 0; i < frames * 2; ++i) pcm[i] = static_cast<int16_t>(std::lround(samples[i] * 32767.0f));
        std::fwrite(pcm.data(), sizeof(int16_t), pcm.size(), file_);
        written_ += frames;
        write_header();
        if (written_ >= limit_) {
            std::fclose(file_);
            file_ = nullptr;
            log_line("audio: audio.wav written (%u frames)", written_);
        }
    }

private:
    void write_header() {
        const uint32_t data_bytes = written_ * 4;
        const uint32_t header[11] = {0x46464952, 36 + data_bytes, 0x45564157, 0x20746D66, 16, 0x00020001,
                                     kOutputRate, kOutputRate * 4, 0x00100004, 0x61746164, data_bytes};
        const long position = std::ftell(file_);
        std::fseek(file_, 0, SEEK_SET);
        std::fwrite(header, sizeof(header), 1, file_);
        std::fseek(file_, position < 44 ? 44 : position, SEEK_SET);
    }

    std::FILE* file_ = nullptr;
    uint32_t limit_ = 0, written_ = 0;
};

// --- XAudio2 output ---

class ChunkCallback : public IXAudio2VoiceCallback {
public:
    explicit ChunkCallback(HANDLE event) : event_(event) {}
    void STDMETHODCALLTYPE OnBufferEnd(void*) override { SetEvent(event_); }
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}

private:
    HANDLE event_;
};

void output_thread(HANDLE ready, bool* ok) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IXAudio2* engine = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* source = nullptr;
    HANDLE chunk_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ChunkCallback callback(chunk_done);

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = 2;
    format.nSamplesPerSec = kOutputRate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = 8;
    format.nAvgBytesPerSec = kOutputRate * 8;

    HRESULT hr = XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (SUCCEEDED(hr)) hr = engine->CreateMasteringVoice(&master, 2, kOutputRate);
    if (SUCCEEDED(hr)) hr = engine->CreateSourceVoice(&source, &format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &callback);
    if (SUCCEEDED(hr)) hr = source->Start();
    *ok = SUCCEEDED(hr);
    if (!*ok) log_line("audio: XAudio2 initialisation failed (%08lX), the game will be silent", hr);
    SetEvent(ready);
    if (!*ok) return;
    log_line("audio: XAudio2 output, stereo %u Hz, %u ms chunks", kOutputRate, kFramesPerChunk * 1000 / kOutputRate);

    std::vector<std::array<float, kFramesPerChunk * 2>> ring(kChunkRing);
    uint32_t next = 0;
    WavDump dump;
    for (;;) {
        XAUDIO2_VOICE_STATE state;
        source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        while (state.BuffersQueued < kQueuedChunks) {
            auto& chunk = ring[next];
            next = (next + 1) % kChunkRing;
            mix(chunk.data(), kFramesPerChunk);
            dump.add(chunk.data(), kFramesPerChunk);
            XAUDIO2_BUFFER buffer{};
            buffer.AudioBytes = kFramesPerChunk * 8;
            buffer.pAudioData = reinterpret_cast<const BYTE*>(chunk.data());
            source->SubmitSourceBuffer(&buffer);
            ++state.BuffersQueued;
        }
        WaitForSingleObject(chunk_done, 20);
    }
}

}  // namespace

std::mutex& audio_lock() { return g_mutex; }

bool start_audio_output() {
    static bool started = false;
    static bool ok = false;
    if (started) return ok;
    started = true;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread(output_thread, ready, &ok).detach();
    WaitForSingleObject(ready, 5000);
    CloseHandle(ready);
    return ok;
}

Voice* create_voice(bool is_stream) {
    auto* voice = new Voice();
    voice->is_stream = is_stream;
    g_voices.push_back(voice);
    return voice;
}

void destroy_voice(Voice* voice) {
    g_voices.erase(std::remove(g_voices.begin(), g_voices.end(), voice), g_voices.end());
    delete voice;
}

void start_envelope(Voice& voice) {
    voice.envelope_frames = 0;
    if (!voice.envelope.enabled) {
        voice.envelope_stage = EnvelopeStage::kOff;
        voice.envelope_level = 1;
        return;
    }
    voice.envelope_stage = static_cast<EnvelopeStage>(std::clamp<uint32_t>(voice.envelope.first_stage, 1, 3));
    voice.envelope_level = voice.envelope_stage == EnvelopeStage::kHold ? 1.0f : 0.0f;
}

void release_voice(Voice& voice) {
    if (voice.envelope_stage == EnvelopeStage::kRelease || voice.envelope_stage == EnvelopeStage::kDone) return;
    voice.release_from = voice.envelope_level;
    voice.envelope_stage = EnvelopeStage::kRelease;
    voice.envelope_frames = 0;
}

int32_t pitch_for_rate(uint32_t samples_per_second) {
    if (samples_per_second == 0) return 0;
    return static_cast<int32_t>(std::lround(4096.0 * std::log2(samples_per_second / static_cast<double>(kOutputRate))));
}

uint32_t bytes_to_frames(const Voice& voice, uint32_t bytes) {
    if (voice.format.format_tag == kFormatXboxAdpcm) {
        const uint32_t block = kAdpcmBlockBytes * std::max<uint32_t>(1, voice.format.channels);
        return bytes / block * kAdpcmBlockFrames;
    }
    return bytes / bytes_per_frame(voice.format);
}

uint32_t frames_to_bytes(const Voice& voice, uint32_t frames) {
    if (voice.format.format_tag == kFormatXboxAdpcm) {
        const uint32_t block = kAdpcmBlockBytes * std::max<uint32_t>(1, voice.format.channels);
        return frames / kAdpcmBlockFrames * block;
    }
    return frames * bytes_per_frame(voice.format);
}

uint32_t play_region_bytes(const Voice& voice) {
    if (voice.play_start >= voice.size) return 0;
    const uint32_t available = voice.size - voice.play_start;
    return voice.play_length ? std::min(voice.play_length, available) : available;
}

void complete_packet(Voice& voice, const StreamPacket& packet, uint32_t status) {
    ++g_statistics.completed_packets;
    if (packet.packet.completed_size) {
        *guest<uint32_t>(packet.packet.completed_size) =
            status == kStatusSuccess ? packet.packet.max_size
                                     : std::min(packet.packet.max_size, frames_to_bytes(voice, packet.consumed_frames));
    }
    if (packet.packet.status) *guest<uint32_t>(packet.packet.status) = status;
    if (voice.callback) {
        g_completed.push_back(
            CompletedPacket{voice.callback, voice.callback_context, packet.packet.completion_event_or_context, status});
    } else if (packet.packet.completion_event_or_context) {
        if (const uint32_t event = object_from_handle(packet.packet.completion_event_or_context)) set_event(event);
    }
}

std::vector<CompletedPacket> take_completed_packets() {
    std::vector<CompletedPacket> completed;
    completed.swap(g_completed);
    return completed;
}

}  // namespace mc3::pc
