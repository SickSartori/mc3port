#pragma once

// The gain a voice reaches the stereo output with. Header-only, without the device, so that tests can check it.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "pc/audio_mixer.h"

namespace xport::pc {

constexpr int32_t kSilentVolume = -10000;
constexpr int32_t kStereoToMono = -602;  // both channels of a stereo voice into a mono mix-in: 1/2 each
// Positioned sounds (engine, wind, traffic, impacts) next to the music. The console's effect stage, which they pass
// through and which is not emulated here, leaves them louder than their mix bins say: about 10 dB, measured against a
// reference and confirmed by listening.
constexpr float kMixInGain = 3.16f;
// A stream XACT plays without mix bins (the pedestrians' voices): about 7 dB, by ear against another player.
constexpr float kStreamWithoutBinsGain = 2.24f;

// DSBCAPS flags that start a buffer without headroom: 3D voices and the mix-in and effect-in buffers.
constexpr uint32_t kNoHeadroomBufferFlags = 0x00000010 | 0x00200000 | 0x00002000 | 0x00080000 | 0x00100000;
constexpr uint32_t kNoHeadroomStreamFlags = 0x00000010;
constexpr int32_t kDefaultHeadroom = 600;  // DSBHEADROOM_DEFAULT_2D: 6 dB

inline int32_t default_headroom(uint32_t create_flags, bool is_stream) {
    return create_flags & (is_stream ? kNoHeadroomStreamFlags : kNoHeadroomBufferFlags) ? 0 : kDefaultHeadroom;
}

inline float decibels_to_gain(int32_t hundredths) {
    if (hundredths <= kSilentVolume) return 0;
    return std::pow(10.0f, static_cast<float>(hundredths) / 2000.0f);
}

// The 5.1 speaker bins fold to stereo (front 1, centre and back 0.5 per side; LFE and effect sends, whose effects are
// not emulated, left out). Bink and XACT send each channel to all six speaker bins at full level; the plain sum was 2.7
// times too loud. The crosstalk bins 6-9 (XTLK front left/right, back left/right) are where XACT routes its positioned
// voices; the DSP's crosstalk effect would put them out on the matching speakers, so they fold like those. Bin 10 is
// the I3DL2 reverb send (no reverb here), 11-30 the effect sends, 31 the submix.
inline void add_mix_bin(uint32_t bin, float gain, float out[2]) {
    switch (bin) {
        case 0: case 6: out[0] += gain; break;                               // front left, XTLK
        case 1: case 7: out[1] += gain; break;                               // front right, XTLK
        case 2: out[0] += gain * 0.5f; out[1] += gain * 0.5f; break;         // centre
        case 4: case 8: out[0] += gain * 0.5f; break;                        // back left, XTLK
        case 5: case 9: out[1] += gain * 0.5f; break;                        // back right, XTLK
        default: break;                                                      // 3 LFE, 10 reverb send, 11+ effect sends and submix
    }
}

// Left/right gain of what enters a mix-in voice. Its input is one mono signal, sent to every bin. The format does not
// matter: XACT creates mix-ins without one, which reads as stereo here, and dealing the bins out to two channels lost
// half of them.
inline void mix_in_gains(const Voice& mix_in, float out[2]) {
    out[0] = out[1] = 0;
    const int32_t volume = mix_in.volume - mix_in.headroom;
    if (mix_in.output) {
        mix_in_gains(*mix_in.output, out);
        const float gain = decibels_to_gain(volume);
        out[0] *= gain;
        out[1] *= gain;
        return;
    }
    if (mix_in.mix_bin_count == 0) {
        out[0] = out[1] = decibels_to_gain(volume);
        return;
    }
    for (int32_t i = 0; i < mix_in.mix_bin_count; ++i) {
        const uint32_t bin = mix_in.mix_bins[i];
        add_mix_bin(bin, decibels_to_gain(volume + (bin < 32 ? mix_in.mix_bin_volumes[bin] : 0)), out);
    }
}

// Left/right gain of each source channel. Headroom lowers the voice before its mix bins.
//
// A voice with an output buffer goes through that mix-in voice alone: its own mix bins do not count, and the mix-in's
// volume and bins place the sound. XACT does this for sounds with a position, so that distance and pan apply to all
// the voices of a sound at once; without it a far car played at full level.
inline void channel_gains(const Voice& voice, float gains[2][2]) {
    std::memset(gains, 0, sizeof(float) * 4);
    const uint32_t channels = std::clamp<uint32_t>(voice.format.channels, 1, 2);
    const int32_t volume = voice.volume - voice.headroom;
    if (voice.output) {
        float output[2];
        mix_in_gains(*voice.output, output);
        const float gain = kMixInGain * decibels_to_gain(volume + (channels == 2 ? kStereoToMono : 0));
        for (uint32_t channel = 0; channel < channels; ++channel) {
            gains[channel][0] = gain * output[0];
            gains[channel][1] = gain * output[1];
        }
        return;
    }
    if (voice.mix_bin_count == 0) {
        // A stream XACT plays without mix bins (the pedestrians' voices) also passes the console's effect stage: at
        // its mix bins' level it was quieter than other sounds.
        const float gain = (voice.is_stream ? kStreamWithoutBinsGain : 1.0f) * decibels_to_gain(volume);
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
        add_mix_bin(bin, decibels_to_gain(volume + (bin < 32 ? voice.mix_bin_volumes[bin] : 0)), gains[channel]);
    }
}

// The pitch a voice plays at. XACT sets a car engine's revs on the mix-in voice the engine's samples play through: the
// samples then move with it, on top of the pitch of their own recording. It stays within what the APU can play.
inline int32_t effective_pitch(const Voice& voice) {
    constexpr int32_t kLowest = -0x7FFF, kHighest = 0x1FFF;
    const int32_t pitch = voice.pitch + (voice.output ? effective_pitch(*voice.output) : 0);
    return std::clamp(pitch, kLowest, kHighest);
}

// False when the output would make a loop; the voice then keeps its own mix bins.
inline bool valid_output(const Voice& voice, const Voice* output) {
    for (const Voice* v = output; v; v = v->output) {
        if (v == &voice) return false;
    }
    return true;
}

}  // namespace xport::pc
