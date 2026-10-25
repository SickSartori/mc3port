#pragma once

// Software replacement for the Xbox audio processor's voice engine, played through XAudio2.
//
// The DirectSound HLE (dsound_hle.cpp) turns buffer and stream calls into voices here. A voice reads its samples
// straight from guest memory, like the APU does, so ring buffers the game rewrites while playing (Bink video
// sound) work unchanged. Mixing is done in software into a stereo 48 kHz stream fed to one XAudio2 voice.

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace mc3::pc {

constexpr uint32_t kOutputRate = 48000;

#pragma pack(push, 1)
struct XWaveFormat {
    uint16_t format_tag;  // 1 = PCM, 0x69 = Xbox ADPCM
    uint16_t channels;
    uint32_t samples_per_second;
    uint32_t average_bytes_per_second;
    uint16_t block_align;
    uint16_t bits_per_sample;
    uint16_t extra_size;
};

// XMEDIAPACKET, the unit of data handed to a stream.
struct XMediaPacket {
    uint32_t buffer;
    uint32_t max_size;
    uint32_t completed_size;  // DWORD*
    uint32_t status;          // DWORD*
    uint32_t completion_event_or_context;
    uint32_t timestamp;       // REFERENCE_TIME*
};
#pragma pack(pop)

struct StreamPacket {
    XMediaPacket packet;
    uint32_t consumed_frames = 0;  // frames, not bytes: ADPCM frames are not a whole number of bytes
};

// A packet the mixer finished (or a flush discarded); its callback runs on a game thread (DirectSoundDoWork).
struct CompletedPacket {
    uint32_t callback;
    uint32_t stream_context;
    uint32_t packet_context;
    uint32_t status;
};

// Amplitude envelope (DSENVELOPEDESC), times in mixer frames. XACT stops its sounds by releasing it: without it a
// stopped sound played its whole sample out and piled up on the next one.
struct Envelope {
    bool enabled = false;
    uint32_t first_stage = 1;  // dwMode: 1 delay, 2 attack, 3 hold
    uint32_t delay = 0, attack = 0, hold = 0, decay = 0, release = 0;
    float sustain = 1;
};
enum class EnvelopeStage : uint8_t { kOff, kDelay, kAttack, kHold, kDecay, kSustain, kRelease, kDone };

// DSFILTERDESC in DLS2 mode: a resonant low-pass, run like the APU's as a state-variable filter at 48 kHz on each
// source channel. XACT sets it on sounds that need it (the river water in Tokyo played unfiltered, as noise).
struct VoiceFilter {
    bool enabled = false;
    float frequency[2] = {1, 1};  // 2 sin(pi fc / 48000)
    float damping[2] = {1, 1};    // 1 / Q
    float low[2] = {}, band[2] = {};
};

struct Voice {
    bool is_stream = false;
    XWaveFormat format{};
    uint32_t create_flags = 0;    // DSBUFFERDESC/DSSTREAMDESC flags, for the voice listing
    uint32_t active_chunks = 0;   // 10 ms chunks mixed since it last started, for the voice listing

    // Buffers: sample data in guest memory, regions in bytes (loop region relative to the play region).
    uint8_t* data = nullptr;
    uint32_t size = 0;
    std::vector<uint8_t> owned_data;  // when DirectSound allocated the memory
    uint32_t play_start = 0;
    uint32_t play_length = 0;  // 0 = to the end
    uint32_t loop_start = 0;
    uint32_t loop_length = 0;  // 0 = whole play region
    double position = 0;       // frames from the start of the play region

    // Streams.
    std::deque<StreamPacket> packets;
    uint32_t max_attached_packets = 0;
    uint32_t callback = 0;
    uint32_t callback_context = 0;
    double stream_fraction = 0;  // fractional frame inside the current packet
    bool end_of_stream = false;  // Discontinuity: stop when the packets run out instead of starving
    // Stopped by an asynchronous flush: packets are handed back unplayed until the stream is set up again. XACT keeps
    // refilling a stopped stream until it sees it is no longer playing; playing them restarted the stopped music.
    bool stopped_by_flush = false;

    bool playing = false;  // streams: from the first packet until flushed or ended (XACT polls it)
    bool looping = false;
    bool paused = false;
    int32_t volume = 0;  // hundredths of a dB, 0 = full scale
    int32_t pitch = 0;   // Xbox pitch: 4096 per octave, 0 = 48 kHz
    int32_t mix_bin_count = 0;
    uint32_t mix_bins[8] = {};
    int32_t mix_bin_volumes[32] = {};  // per mix bin, hundredths of a dB

    // Decoding cache for ADPCM: the block last decoded.
    int64_t decoded_block = -1;
    std::vector<int16_t> decoded;  // 64 samples per channel, interleaved

    // Gains used at the end of the previous chunk: volume changes ramp from them instead of jumping (a jump every
    // 10 ms is an audible click when the game moves volumes continuously, as it does for engines).
    float last_gains[2][2] = {};
    bool has_last_gains = false;

    VoiceFilter filter;

    Envelope envelope;
    EnvelopeStage envelope_stage = EnvelopeStage::kOff;  // kOff: full level
    uint32_t envelope_frames = 0;                        // frames spent in the current stage
    float envelope_level = 1;
    float release_from = 1;  // level when the release started
};

// All voices and the device. Everything below requires audio_lock() held, except start_audio_output().
std::mutex& audio_lock();
bool start_audio_output();
Voice* create_voice(bool is_stream);
void destroy_voice(Voice* voice);

// Envelope control: start (Play) restarts it from its first stage, release fades the voice out over the release time
// and then stops it (a stream is flushed).
void start_envelope(Voice& voice);
void release_voice(Voice& voice);

// Pitch for a sample rate, like XAudioCalculatePitch: 0 at 48 kHz, 4096 per octave.
int32_t pitch_for_rate(uint32_t samples_per_second);

// Bytes <-> frames for the voice's format (ADPCM: 36 bytes per channel give 64 frames).
uint32_t bytes_to_frames(const Voice& voice, uint32_t bytes);
uint32_t frames_to_bytes(const Voice& voice, uint32_t frames);
uint32_t play_region_bytes(const Voice& voice);

// Packets finished since the last call, for DirectSoundDoWork to report to the game.
std::vector<CompletedPacket> take_completed_packets();
void complete_packet(Voice& voice, const StreamPacket& packet, uint32_t status);

}  // namespace mc3::pc
