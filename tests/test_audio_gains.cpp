// The gain a voice reaches the output with: headroom, mix bins, and a voice played through a mix-in voice.

#include <cmath>
#include <cstdint>

#include "pc/audio_gains.h"
#include "test.h"

using namespace mc3::pc;

namespace {

bool near(float a, float b) { return std::abs(a - b) < 0.0005f; }

Voice mono(int32_t volume, int32_t headroom = 0) {
    Voice voice;
    voice.format.channels = 1;
    voice.volume = volume;
    voice.headroom = headroom;
    return voice;
}

void route(Voice& voice, uint32_t bin, int32_t volume) {
    voice.mix_bins[voice.mix_bin_count++] = bin;
    voice.mix_bin_volumes[bin] = volume;
}

void test_headroom_defaults() {
    CHECK(default_headroom(0x00400000, false) == 600);  // what XACT's buffers are created with
    CHECK(default_headroom(0x00000010, false) == 0);    // 3D
    CHECK(default_headroom(0x00002000, false) == 0);    // mix-in
    CHECK(default_headroom(0x00080000, false) == 0);    // effect-in
    CHECK(default_headroom(0x20400000, true) == 600);
    CHECK(default_headroom(0x00000010, true) == 0);
}

void test_direct_voice() {
    float gains[2][2];
    Voice plain = mono(-193, 600);  // -7.93 dB: 0.401
    channel_gains(plain, gains);
    CHECK(near(gains[0][0], 0.4013f) && near(gains[0][1], 0.4013f));

    Voice silent = mono(-10000);
    channel_gains(silent, gains);
    CHECK(gains[0][0] == 0 && gains[0][1] == 0);

    Voice bins = mono(0);
    route(bins, 0, 0);
    route(bins, 2, -2000);
    channel_gains(bins, gains);
    CHECK(near(gains[0][0], 1.05f) && near(gains[0][1], 0.05f));
}

// The numbers of a car's wind in the city: the mix-in takes it down with the distance.
void test_through_mix_in() {
    Voice mix_in = mono(0);
    route(mix_in, 6, -2116);
    route(mix_in, 7, -2114);
    route(mix_in, 2, -1979);

    Voice wind = mono(-193, 600);
    route(wind, 0, 0);  // does not count once it plays through the mix-in
    wind.output = &mix_in;
    float gains[2][2];
    channel_gains(wind, gains);
    const float left = kMixInGain * 0.4013f * (0.0875f + 0.0513f);
    CHECK(near(gains[0][0], left) && near(gains[0][1], left));
    CHECK(gains[0][0] < 0.08f * kMixInGain);

    mix_in.volume = -10000;
    channel_gains(wind, gains);
    CHECK(gains[0][0] == 0);
}

// XACT creates mix-ins without a format, which reads as stereo: the bins must still all count, in any order.
void test_mix_in_with_stereo_format() {
    for (const uint32_t order : {0u, 1u}) {
        Voice mix_in = mono(0);
        mix_in.format.channels = 2;
        route(mix_in, 6, -1062);
        route(mix_in, order ? 8 : 7, order ? -2513 : -1062);
        route(mix_in, order ? 7 : 2, order ? -1062 : -926);
        route(mix_in, order ? 9 : 8, -2513);
        route(mix_in, order ? 2 : 9, order ? -926 : -2513);

        Voice engine = mono(-1643, 600);
        engine.output = &mix_in;
        float gains[2][2];
        channel_gains(engine, gains);
        const float side = kMixInGain * 0.0757f * (0.2945f + 0.3442f * 0.5f + 0.0556f * 0.5f);
        CHECK(near(gains[0][0], side));
        CHECK(near(gains[0][1], side));
    }
}

void test_stereo_into_mix_in() {
    Voice mix_in = mono(0);
    route(mix_in, 0, 0);
    route(mix_in, 1, 0);
    Voice stereo = mono(0);
    stereo.format.channels = 2;
    stereo.output = &mix_in;
    float gains[2][2];
    channel_gains(stereo, gains);
    const float half = 0.5f * kMixInGain;
    CHECK(near(gains[0][0], half) && near(gains[1][0], half));
    CHECK(near(gains[0][1], half) && near(gains[1][1], half));
}

// The engine's revs are set on the mix-in; the sample keeps the pitch of its recording on top.
void test_pitch_through_mix_in() {
    Voice mix_in = mono(0);
    Voice engine = mono(0);
    engine.pitch = -3185;
    CHECK(effective_pitch(engine) == -3185);
    engine.output = &mix_in;
    mix_in.pitch = 1000;
    CHECK(effective_pitch(engine) == -2185);
    mix_in.pitch = -1000;
    CHECK(effective_pitch(engine) == -4185);
    mix_in.pitch = 0x1FFF;
    engine.pitch = 0x1000;
    CHECK(effective_pitch(engine) == 0x1FFF);
}

void test_no_output_loops() {
    Voice a = mono(0), b = mono(0), c = mono(0);
    CHECK(valid_output(a, nullptr));
    CHECK(valid_output(a, &b));
    CHECK(!valid_output(a, &a));
    b.output = &a;
    CHECK(!valid_output(a, &b));
    c.output = &b;
    CHECK(!valid_output(a, &c));
}

}  // namespace

// "No more data" ends a stream at once only if it has had data or was flushed; one waiting for its first read goes on.
void test_discontinuity() {
    Voice waiting;
    waiting.is_stream = true;
    CHECK(!stops_at_discontinuity(waiting));
    Voice fed = waiting;
    fed.stream_fed = true;
    CHECK(stops_at_discontinuity(fed));
    Voice flushed = waiting;
    flushed.stopped_by_flush = true;
    CHECK(stops_at_discontinuity(flushed));
    Voice with_data = fed;
    with_data.packets.push_back(StreamPacket{});
    CHECK(!stops_at_discontinuity(with_data));
}

// A stream without mix bins (the pedestrians' voices) has the effect stage's gain; a buffer without them has none.
void test_stream_without_mix_bins() {
    Voice stream = mono(0);
    stream.is_stream = true;
    float gains[2][2];
    channel_gains(stream, gains);
    CHECK(near(gains[0][0], kStreamWithoutBinsGain) && near(gains[0][1], kStreamWithoutBinsGain));
    channel_gains(mono(0), gains);
    CHECK(near(gains[0][0], 1.0f) && near(gains[0][1], 1.0f));
}

int main() {
    test_headroom_defaults();
    test_direct_voice();
    test_through_mix_in();
    test_mix_in_with_stereo_format();
    test_stereo_into_mix_in();
    test_pitch_through_mix_in();
    test_no_output_loops();
    test_discontinuity();
    test_stream_without_mix_bins();
    return test_result();
}
