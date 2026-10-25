// DirectSound replaced at its API boundary.
//
// Every DirectSound function the game, XACT, Bink and the other libraries call from outside the library (found by
// scanning the XBE, see docs/ANALISI_XBE.md) is redirected here with the same stdcall prototype. Buffers and streams
// become voices of the software mixer (audio_mixer.cpp), which plays through XAudio2. XACT and Bink keep running
// unchanged on top. Functions whose exact role is not yet confirmed accept their arguments and succeed; they are
// marked "unidentified" and log their first calls.

#include "pc/dsound_hle.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <mutex>

#include "pc/audio_mixer.h"
#include "pc/exec_memory.h"
#include "pc/log.h"
#include "pc/xbox_types.h"

namespace mc3::pc {
namespace {

using HRESULT32 = int32_t;
constexpr HRESULT32 kOk = 0;
constexpr HRESULT32 kFail = static_cast<HRESULT32>(0x80004005);
constexpr HRESULT32 kInvalidCall = static_cast<HRESULT32>(0x88780032);
constexpr uint32_t kPacketPending = 0x8000000A;
constexpr uint32_t kPacketFlushed = 0x80004004;

constexpr uint32_t kPlayLooping = 0x1;
constexpr uint32_t kPlayFromStart = 0x2;
constexpr uint32_t kStopExReleaseWaveform = 0x2;
constexpr uint32_t kStatusPlaying = 0x1;
constexpr uint32_t kStatusPaused = 0x2;
constexpr uint32_t kStatusLooping = 0x4;
constexpr uint32_t kLockFromWriteCursor = 0x1;
constexpr uint32_t kLockEntireBuffer = 0x2;
constexpr uint32_t kXmoAcceptInputData = 0x1;

#pragma pack(push, 1)
struct XDsBufferDesc {
    uint32_t size;
    uint32_t flags;
    uint32_t buffer_bytes;
    uint32_t format;  // XWaveFormat*
    uint32_t mix_bins;
    uint32_t input_mix_bin;
};
struct XDsStreamDesc {
    uint32_t flags;
    uint32_t max_attached_packets;
    uint32_t format;
    uint32_t callback;
    uint32_t context;
    uint32_t mix_bins;
};
struct XDsMixBins {
    uint32_t count;
    uint32_t pairs;  // {DWORD bin; LONG volume}[count]
};
#pragma pack(pop)

// Objects handed to the game. The vtable comes first: IUnknown for DirectSound and buffers, XMediaObject for
// streams (whose Process/GetStatus/Flush the libraries call through it).
struct DsObject {
    const void* const* vtable;
    uint32_t references;
    Voice* voice;
};

DsObject g_direct_sound{};
const XWaveFormat kDefaultFormat{1, 2, 48000, 192000, 4, 16, 0};

uint32_t unidentified_calls = 0;

void log_unidentified(const char* name, uint32_t a = 0, uint32_t b = 0) {
    if (unidentified_calls++ < 40) log_line("dsound: unidentified %s(%08X, %08X) accepted", name, a, b);
}

Voice* voice_of(uint32_t object) { return object ? guest<DsObject>(object)->voice : nullptr; }

void apply_format(Voice& voice, uint32_t format_address) {
    voice.format = format_address ? *guest<XWaveFormat>(format_address) : kDefaultFormat;
    voice.pitch = pitch_for_rate(voice.format.samples_per_second);
    voice.decoded_block = -1;
    static uint32_t logged = 0;
    if (logged++ < 60) {
        log_line("dsound: voice %p format tag %X, %u channels, %u Hz, %u bits, block %u", static_cast<void*>(&voice),
                 voice.format.format_tag, voice.format.channels, voice.format.samples_per_second,
                 voice.format.bits_per_sample, voice.format.block_align);
    }
}

void apply_mix_bins(Voice& voice, uint32_t mix_bins_address, bool routing) {
    if (!mix_bins_address) return;
    // A voice created without mix bins gets them from its first SetMixBinVolumes: XACT pans its voices that way,
    // and without a routing they played at full level in the middle.
    if (!routing && voice.mix_bin_count == 0) routing = true;
    const XDsMixBins& bins = *guest<XDsMixBins>(mix_bins_address);
    const uint32_t count = std::min<uint32_t>(bins.count, 8);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t bin = guest<uint32_t>(bins.pairs)[i * 2];
        const int32_t volume = static_cast<int32_t>(guest<uint32_t>(bins.pairs)[i * 2 + 1]);
        if (routing) voice.mix_bins[i] = bin;
        if (bin < 32) voice.mix_bin_volumes[bin] = volume;
    }
    if (routing) voice.mix_bin_count = static_cast<int32_t>(count);
    static uint32_t logged = 0;
    if (logged++ < 40) {
        std::string list;
        for (uint32_t i = 0; i < count; ++i) {
            char entry[32];
            std::snprintf(entry, sizeof(entry), " %u:%d", guest<uint32_t>(bins.pairs)[i * 2],
                          static_cast<int32_t>(guest<uint32_t>(bins.pairs)[i * 2 + 1]));
            list += entry;
        }
        log_line("dsound: voice %p %s mix bins (%u channels):%s", static_cast<void*>(&voice),
                 routing ? "routes to" : "volumes", voice.format.channels, list.c_str());
    }
}

uint32_t buffer_play_cursor(const Voice& voice) {
    return voice.play_start + frames_to_bytes(voice, static_cast<uint32_t>(voice.position));
}

// --- IUnknown for DirectSound and buffers ---

HRESULT32 __stdcall QueryInterface(uint32_t, uint32_t, uint32_t* out) {
    if (out) *out = 0;
    return static_cast<HRESULT32>(0x80004002);  // E_NOINTERFACE
}

uint32_t __stdcall DirectSoundAddRef(uint32_t self) { return ++guest<DsObject>(self)->references; }
uint32_t __stdcall DirectSoundRelease(uint32_t self) {
    DsObject* object = guest<DsObject>(self);
    return object->references ? --object->references : 0;  // the device lives as long as the game
}

uint32_t __stdcall BufferAddRef(uint32_t self) { return ++guest<DsObject>(self)->references; }
uint32_t __stdcall BufferRelease(uint32_t self) {
    std::lock_guard<std::mutex> lock(audio_lock());
    DsObject* object = guest<DsObject>(self);
    if (--object->references) return object->references;
    destroy_voice(object->voice);
    delete object;
    return 0;
}

const void* const kDirectSoundVtable[] = {reinterpret_cast<const void*>(&QueryInterface),
                                          reinterpret_cast<const void*>(&DirectSoundAddRef),
                                          reinterpret_cast<const void*>(&DirectSoundRelease)};
const void* const kBufferVtable[] = {reinterpret_cast<const void*>(&QueryInterface),
                                     reinterpret_cast<const void*>(&BufferAddRef),
                                     reinterpret_cast<const void*>(&BufferRelease)};

// --- streams (XMediaObject) ---

void flush_stream(Voice& voice) {
    for (const StreamPacket& packet : voice.packets) complete_packet(voice, packet, kPacketFlushed);
    voice.packets.clear();
    voice.stream_fraction = 0;
    voice.decoded_block = -1;
}

uint32_t __stdcall StreamAddRef(uint32_t self) { return ++guest<DsObject>(self)->references; }
uint32_t __stdcall StreamRelease(uint32_t self) {
    std::lock_guard<std::mutex> lock(audio_lock());
    DsObject* object = guest<DsObject>(self);
    if (--object->references) return object->references;
    flush_stream(*object->voice);
    destroy_voice(object->voice);
    delete object;
    return 0;
}

HRESULT32 __stdcall StreamGetInfo(uint32_t self, uint32_t* info) {
    constexpr uint32_t kFixedPacketAlignment = 0x2;
    std::lock_guard<std::mutex> lock(audio_lock());
    const Voice* voice = voice_of(self);
    info[0] = kFixedPacketAlignment;
    info[1] = std::max<uint32_t>(1, voice->format.block_align);
    info[2] = 0;
    info[3] = 0;
    return kOk;
}

HRESULT32 __stdcall StreamGetStatus(uint32_t self, uint32_t* status) {
    std::lock_guard<std::mutex> lock(audio_lock());
    const Voice* voice = voice_of(self);
    *status = voice->packets.size() < std::max<uint32_t>(1, voice->max_attached_packets) ? kXmoAcceptInputData : 0;
    return kOk;
}

HRESULT32 __stdcall StreamProcess(uint32_t self, const XMediaPacket* input, const XMediaPacket* /*output*/) {
    if (!input) return kInvalidCall;
    std::lock_guard<std::mutex> lock(audio_lock());
    Voice* voice = voice_of(self);
    if (input->completed_size) *guest<uint32_t>(input->completed_size) = 0;
    if (input->status) *guest<uint32_t>(input->status) = kPacketPending;
    voice->packets.push_back(StreamPacket{*input, 0});
    return kOk;
}

HRESULT32 __stdcall StreamDiscontinuity(uint32_t) { return kOk; }

HRESULT32 __stdcall StreamFlush(uint32_t self) {
    std::lock_guard<std::mutex> lock(audio_lock());
    flush_stream(*voice_of(self));
    return kOk;
}

const void* const kStreamVtable[] = {
    reinterpret_cast<const void*>(&StreamAddRef),       reinterpret_cast<const void*>(&StreamRelease),
    reinterpret_cast<const void*>(&StreamGetInfo),      reinterpret_cast<const void*>(&StreamGetStatus),
    reinterpret_cast<const void*>(&StreamProcess),      reinterpret_cast<const void*>(&StreamDiscontinuity),
    reinterpret_cast<const void*>(&StreamFlush)};

// --- global functions ---

HRESULT32 __stdcall DirectSoundCreate(uint32_t /*guid*/, uint32_t* direct_sound, uint32_t /*outer*/) {
    static std::once_flag once;
    std::call_once(once, [] {
        start_audio_output();
        g_direct_sound.vtable = kDirectSoundVtable;
    });
    ++g_direct_sound.references;
    *direct_sound = address_of(&g_direct_sound);
    return kOk;
}

HRESULT32 create_stream(const XDsStreamDesc* desc, uint32_t* stream) {
    std::lock_guard<std::mutex> lock(audio_lock());
    auto* object = new DsObject{kStreamVtable, 1, create_voice(true)};
    apply_format(*object->voice, desc->format);
    object->voice->max_attached_packets = desc->max_attached_packets;
    object->voice->callback = desc->callback;
    object->voice->callback_context = desc->context;
    apply_mix_bins(*object->voice, desc->mix_bins, true);
    *stream = address_of(object);
    return kOk;
}

HRESULT32 __stdcall DirectSoundCreateStream(const XDsStreamDesc* desc, uint32_t* stream) {
    uint32_t direct_sound = 0;
    DirectSoundCreate(0, &direct_sound, 0);
    return create_stream(desc, stream);
}

// Delivers stream packet callbacks on the calling game thread, as the Xbox does from its DirectSound work.
void __stdcall DirectSoundDoWork() {
    std::vector<CompletedPacket> completed;
    {
        std::lock_guard<std::mutex> lock(audio_lock());
        completed = take_completed_packets();
    }
    using Callback = void(__stdcall*)(uint32_t stream_context, uint32_t packet_context, uint32_t status);
    for (const CompletedPacket& packet : completed) {
        reinterpret_cast<Callback>(static_cast<uintptr_t>(packet.callback))(packet.stream_context,
                                                                            packet.packet_context, packet.status);
    }
}

// --- IDirectSound ---

HRESULT32 __stdcall DsGetCaps(uint32_t, uint32_t* caps) {
    caps[0] = 256;   // free 2D voices
    caps[1] = 64;    // free 3D voices
    caps[2] = 2047;  // free buffer SGEs
    caps[3] = 0;     // memory allocated
    return kOk;
}

// The effects image (dsstdfx.bin) would be loaded into the audio DSPs. Effects are not emulated: the game gets a
// description with zeroed state blocks it may write parameters into.
HRESULT32 __stdcall DsDownloadEffectsImage(uint32_t, uint32_t, uint32_t size, uint32_t, uint32_t* image_desc) {
    constexpr uint32_t kEffects = 32;
    constexpr uint32_t kStateBytes = 0x1000;
    if (image_desc) {
        // DSEFFECTIMAGEDESC: count, scratch size, then 8 dwords per effect (code, state, Y memory, scratch).
        static std::vector<uint32_t> desc(2 + kEffects * 8);
        static std::vector<uint8_t> state(kEffects * kStateBytes);
        desc[0] = kEffects;
        for (uint32_t i = 0; i < kEffects; ++i) {
            desc[2 + i * 8 + 2] = address_of(state.data() + i * kStateBytes);
            desc[2 + i * 8 + 3] = kStateBytes;
        }
        *image_desc = address_of(desc.data());
    }
    log_line("dsound: effects image of %u bytes accepted (effects are not emulated)", size);
    return kOk;
}

HRESULT32 __stdcall DsSetEffectData(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return kOk; }
HRESULT32 __stdcall DsEnableHeadphones(uint32_t, uint32_t) { return kOk; }
HRESULT32 __stdcall DsSetMixBinHeadroom(uint32_t, uint32_t bin, uint32_t headroom) {
    static uint32_t logged = 0;
    if (logged++ < 40) log_line("dsound: mix bin %u headroom %u", bin, headroom);
    return kOk;
}
HRESULT32 __stdcall DsCommitDeferredSettings(uint32_t) { return kOk; }
HRESULT32 __stdcall DsSetI3DL2Listener(uint32_t, uint32_t, uint32_t) { return kOk; }

HRESULT32 __stdcall DsGetOutputLevels(uint32_t, uint32_t* levels, uint32_t) {
    if (levels) std::memset(levels, 0, 16 * sizeof(uint32_t));
    return kOk;
}

HRESULT32 __stdcall DsCreateSoundBuffer(uint32_t, const XDsBufferDesc* desc, uint32_t* buffer, uint32_t) {
    std::lock_guard<std::mutex> lock(audio_lock());
    auto* object = new DsObject{kBufferVtable, 1, create_voice(false)};
    Voice& voice = *object->voice;
    static uint32_t logged = 0;
    if (logged++ < 30) {
        log_line("dsound: create buffer %p: size %u flags %08X bytes %u format %08X mix bins %08X input %08X",
                 static_cast<void*>(&voice), desc->size, desc->flags, desc->buffer_bytes, desc->format, desc->mix_bins,
                 desc->input_mix_bin);
    }
    apply_format(voice, desc->format);
    if (desc->buffer_bytes) {
        voice.owned_data.assign(desc->buffer_bytes, 0);
        voice.data = voice.owned_data.data();
        voice.size = desc->buffer_bytes;
    }
    apply_mix_bins(voice, desc->mix_bins, true);
    *buffer = address_of(object);
    return kOk;
}

HRESULT32 __stdcall DsCreateSoundStream(uint32_t, const XDsStreamDesc* desc, uint32_t* stream, uint32_t) {
    return create_stream(desc, stream);
}

// --- voice parameters, shared by buffers and streams ---

HRESULT32 __stdcall VoiceSetVolume(uint32_t self, int32_t volume) {
    std::lock_guard<std::mutex> lock(audio_lock());
    voice_of(self)->volume = volume;
    return kOk;
}

HRESULT32 __stdcall VoiceSetPitch(uint32_t self, int32_t pitch) {
    std::lock_guard<std::mutex> lock(audio_lock());
    voice_of(self)->pitch = pitch;
    return kOk;
}

HRESULT32 __stdcall VoiceSetFrequency(uint32_t self, uint32_t frequency) {
    std::lock_guard<std::mutex> lock(audio_lock());
    Voice* voice = voice_of(self);
    voice->pitch = pitch_for_rate(frequency ? frequency : voice->format.samples_per_second);
    return kOk;
}

// DSVOICEPROPS: what the voice is actually playing with (XACT reads it back to adjust volumes).
struct XDsVoiceProps {
    uint32_t mix_bin_count;
    struct {
        uint32_t mix_bin;
        int32_t volume;
    } mix_bins[8];
    int32_t pitch;
    int32_t i3dl2_direct_volume;
    int32_t i3dl2_room_volume;
};

HRESULT32 __stdcall VoiceGetVoiceProperties(uint32_t self, XDsVoiceProps* props) {
    std::lock_guard<std::mutex> lock(audio_lock());
    const Voice* voice = voice_of(self);
    std::memset(props, 0, sizeof(*props));
    props->mix_bin_count = static_cast<uint32_t>(std::clamp(voice->mix_bin_count, 0, 8));
    for (uint32_t i = 0; i < props->mix_bin_count; ++i) {
        const uint32_t bin = voice->mix_bins[i];
        props->mix_bins[i].mix_bin = bin;
        props->mix_bins[i].volume = std::max(-10000, voice->volume + (bin < 32 ? voice->mix_bin_volumes[bin] : 0));
    }
    props->pitch = voice->pitch;
    props->i3dl2_direct_volume = props->i3dl2_room_volume = -10000;
    return kOk;
}

HRESULT32 __stdcall VoiceSetMixBins(uint32_t self, uint32_t mix_bins) {
    std::lock_guard<std::mutex> lock(audio_lock());
    apply_mix_bins(*voice_of(self), mix_bins, true);
    return kOk;
}

HRESULT32 __stdcall VoiceSetMixBinVolumes(uint32_t self, uint32_t mix_bins) {
    std::lock_guard<std::mutex> lock(audio_lock());
    apply_mix_bins(*voice_of(self), mix_bins, false);
    return kOk;
}

HRESULT32 __stdcall VoiceSetFormat(uint32_t self, uint32_t format) {
    std::lock_guard<std::mutex> lock(audio_lock());
    apply_format(*voice_of(self), format);
    return kOk;
}

// LFO, envelope, filter and headroom shape the sound on the APU; not emulated yet.
HRESULT32 __stdcall VoiceSetLfo(uint32_t, uint32_t) { return kOk; }
HRESULT32 __stdcall VoiceSetEnvelope(uint32_t, uint32_t) { return kOk; }
HRESULT32 __stdcall VoiceSetFilter(uint32_t, uint32_t) { return kOk; }
HRESULT32 __stdcall VoiceSetHeadroom(uint32_t self, uint32_t headroom) {
    static uint32_t logged = 0;
    if (logged++ < 40) log_line("dsound: voice %p headroom %u", static_cast<void*>(voice_of(self)), headroom);
    return kOk;
}
HRESULT32 __stdcall VoiceSetOutputBuffer(uint32_t, uint32_t) { return kOk; }

HRESULT32 __stdcall VoiceUnidentified2a(uint32_t self, uint32_t a) { log_unidentified("voice 0x0C94", self, a); return kOk; }
HRESULT32 __stdcall VoiceUnidentified2b(uint32_t self, uint32_t a) { log_unidentified("voice 0x0DC5", self, a); return kOk; }
HRESULT32 __stdcall Voice3dParameters5(uint32_t self, uint32_t a, uint32_t, uint32_t, uint32_t) {
    log_unidentified("stream 3D 0x1627", self, a);
    return kOk;
}
HRESULT32 __stdcall Voice3dParameters2(uint32_t self, uint32_t a) { log_unidentified("stream 3D 0x2C7C", self, a); return kOk; }

// --- buffers ---

HRESULT32 __stdcall BufferPlay(uint32_t self, uint32_t, uint32_t, uint32_t flags) {
    std::lock_guard<std::mutex> lock(audio_lock());
    Voice* voice = voice_of(self);
    if (flags & kPlayFromStart) voice->position = 0;
    voice->looping = (flags & kPlayLooping) != 0;
    voice->playing = true;
    voice->paused = false;
    return kOk;
}

HRESULT32 __stdcall BufferStop(uint32_t self) {
    std::lock_guard<std::mutex> lock(audio_lock());
    voice_of(self)->playing = false;
    return kOk;
}

HRESULT32 __stdcall BufferStopEx(uint32_t self, uint32_t, uint32_t, uint32_t flags) {
    std::lock_guard<std::mutex> lock(audio_lock());
    Voice* voice = voice_of(self);
    if (flags & kStopExReleaseWaveform) {
        voice->looping = false;  // play out to the end of the region
    } else {
        voice->playing = false;
    }
    return kOk;
}

HRESULT32 __stdcall BufferSetPlayRegion(uint32_t self, uint32_t start, uint32_t length) {
    std::lock_guard<std::mutex> lock(audio_lock());
    Voice* voice = voice_of(self);
    voice->play_start = start;
    voice->play_length = length;
    voice->loop_start = 0;
    voice->loop_length = 0;
    return kOk;
}

HRESULT32 __stdcall BufferSetLoopRegion(uint32_t self, uint32_t start, uint32_t length) {
    std::lock_guard<std::mutex> lock(audio_lock());
    Voice* voice = voice_of(self);
    voice->loop_start = start;
    voice->loop_length = length;
    return kOk;
}

HRESULT32 __stdcall BufferGetStatus(uint32_t self, uint32_t* status) {
    std::lock_guard<std::mutex> lock(audio_lock());
    const Voice* voice = voice_of(self);
    *status = (voice->playing ? kStatusPlaying : 0) | (voice->playing && voice->looping ? kStatusLooping : 0) |
              (voice->paused ? kStatusPaused : 0);
    return kOk;
}

// The write cursor runs one APU frame (32 samples) ahead of the play cursor, like the hardware. A bigger lead
// (it was 10 ms) traps Bink: it fills silence until write - play is small and never re-reads the cursors.
uint32_t write_cursor_of(const Voice& voice, uint32_t play) {
    constexpr uint32_t kApuFrameSamples = 32;
    const uint32_t ahead = frames_to_bytes(voice, kApuFrameSamples);
    return voice.size ? (play + ahead) % voice.size : play;
}

HRESULT32 __stdcall BufferGetCurrentPosition(uint32_t self, uint32_t* play_cursor, uint32_t* write_cursor) {
    std::lock_guard<std::mutex> lock(audio_lock());
    const Voice* voice = voice_of(self);
    const uint32_t play = buffer_play_cursor(*voice);
    if (play_cursor) *play_cursor = play;
    if (write_cursor) *write_cursor = write_cursor_of(*voice, play);
    return kOk;
}

HRESULT32 __stdcall BufferSetCurrentPosition(uint32_t self, uint32_t offset) {
    std::lock_guard<std::mutex> lock(audio_lock());
    Voice* voice = voice_of(self);
    voice->position = offset > voice->play_start ? bytes_to_frames(*voice, offset - voice->play_start) : 0;
    return kOk;
}

HRESULT32 __stdcall BufferLock(uint32_t self, uint32_t offset, uint32_t bytes, uint32_t* pointer1, uint32_t* bytes1,
                               uint32_t* pointer2, uint32_t* bytes2, uint32_t flags) {
    std::lock_guard<std::mutex> lock(audio_lock());
    Voice* voice = voice_of(self);
    if (!voice->data || voice->size == 0) return kInvalidCall;
    if (flags & kLockEntireBuffer) bytes = voice->size;
    if (flags & kLockFromWriteCursor) offset = write_cursor_of(*voice, buffer_play_cursor(*voice));
    offset %= voice->size;
    bytes = std::min(bytes, voice->size);
    const uint32_t first = std::min(bytes, voice->size - offset);
    *pointer1 = address_of(voice->data + offset);
    *bytes1 = first;
    if (pointer2) *pointer2 = first < bytes ? address_of(voice->data) : 0;
    if (bytes2) *bytes2 = bytes - first;
    static uint32_t logged = 0;
    if (logged++ < 40) {
        log_line("dsound: lock voice %p offset %u bytes %u flags %X (play %u, size %u)", static_cast<void*>(voice),
                 offset, bytes, flags, buffer_play_cursor(*voice), voice->size);
    }
    return kOk;
}

HRESULT32 __stdcall BufferUnlock(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return kOk; }

HRESULT32 __stdcall BufferSetBufferData(uint32_t self, uint32_t data, uint32_t bytes) {
    std::lock_guard<std::mutex> lock(audio_lock());
    Voice* voice = voice_of(self);
    static uint32_t logged = 0;
    if (logged++ < 30) {
        log_line("dsound: set buffer data %p: data %08X bytes %u (format tag %X, %u ch, %u Hz)",
                 static_cast<void*>(voice), data, bytes, voice->format.format_tag, voice->format.channels,
                 voice->format.samples_per_second);
    }
    voice->owned_data.clear();
    voice->data = data ? guest<uint8_t>(data) : nullptr;
    voice->size = data ? bytes : 0;
    voice->play_start = voice->play_length = voice->loop_start = voice->loop_length = 0;
    voice->position = 0;
    voice->decoded_block = -1;
    return kOk;
}

// --- streams: exported functions ---

HRESULT32 __stdcall StreamPause(uint32_t self, uint32_t pause) {
    std::lock_guard<std::mutex> lock(audio_lock());
    voice_of(self)->paused = pause != 0;
    return kOk;
}

HRESULT32 __stdcall StreamFlushEx(uint32_t self, uint32_t, uint32_t, uint32_t) { return StreamFlush(self); }

struct Redirect {
    uint32_t address;
    const void* function;
    const char* name;
};

#define R(address, function) {address, reinterpret_cast<const void*>(&function), #function}

const Redirect kRedirects[] = {
    // Global functions.
    R(0x3C38D3, DirectSoundCreate),
    R(0x3C391A, DirectSoundCreateStream),
    R(0x3C22EC, DirectSoundDoWork),
    // IDirectSound.
    R(0x3C06C1, DirectSoundRelease),
    R(0x3C1FAB, DsGetCaps),
    R(0x3C1FC7, DsDownloadEffectsImage),
    R(0x3C1FEE, DsSetEffectData),
    R(0x3C2018, DsEnableHeadphones),
    R(0x3C2034, DsSetMixBinHeadroom),
    R(0x3C2054, DsGetOutputLevels),
    R(0x3C345D, DsCommitDeferredSettings),
    R(0x3C36C5, DsCreateSoundBuffer),
    R(0x3C36E9, DsCreateSoundStream),
    R(0x3C370D, DsSetI3DL2Listener),
    // IDirectSoundBuffer.
    R(0x3C06DC, BufferRelease),
    R(0x3C06D7, BufferUnlock),
    R(0x3C2074, VoiceSetVolume),
    R(0x3C2090, VoiceSetPitch),
    R(0x3C20AC, VoiceSetLfo),
    R(0x3C20C8, VoiceSetEnvelope),
    R(0x3C20E4, VoiceSetFilter),
    R(0x3C2100, VoiceSetHeadroom),
    R(0x3C211C, VoiceSetMixBinVolumes),
    R(0x3C2138, BufferPlay),
    R(0x3C215C, BufferStop),
    R(0x3C2174, BufferStopEx),
    R(0x3C2198, BufferSetLoopRegion),
    R(0x3C21B8, BufferGetStatus),
    R(0x3C21D4, BufferGetCurrentPosition),
    R(0x3C21F4, BufferSetCurrentPosition),
    R(0x3C2210, BufferLock),
    R(0x3C2240, VoiceGetVoiceProperties),
    R(0x3C225C, VoiceUnidentified2a),
    R(0x3C2278, VoiceUnidentified2b),
    R(0x3C28BF, VoiceSetFrequency),
    R(0x3C28DB, VoiceSetOutputBuffer),
    R(0x3C28F7, VoiceSetMixBins),  // Bink passes a DSMIXBINS here (count, pairs): not SetFormat
    R(0x3C2913, BufferSetPlayRegion),
    R(0x3C2F5C, VoiceSetFormat),  // XACT's real formats (mono 48 kHz...): a WAVEFORMATEX, internal 0x2AD9
    R(0x3C2F78, BufferSetBufferData),
    // IDirectSoundStream (the XMediaObject methods go through the stream's vtable).
    R(0x3C2294, VoiceSetVolume),
    R(0x3C2299, VoiceSetPitch),
    R(0x3C229E, VoiceSetLfo),
    R(0x3C22A3, VoiceSetEnvelope),
    R(0x3C22A8, VoiceSetFilter),
    R(0x3C22AD, VoiceSetHeadroom),
    R(0x3C22B2, VoiceSetMixBinVolumes),
    R(0x3C22B7, StreamPause),
    R(0x3C22BC, StreamFlushEx),
    R(0x3C22D4, VoiceGetVoiceProperties),
    R(0x3C22D9, VoiceUnidentified2a),
    R(0x3C22DE, VoiceUnidentified2b),
    R(0x3C22E3, Voice3dParameters5),
    R(0x3C2933, VoiceSetOutputBuffer),
    R(0x3C2938, VoiceSetMixBins),
    R(0x3C2F98, VoiceSetFormat),
    R(0x3C2F9D, Voice3dParameters2),
};

#undef R

}  // namespace

void install_dsound_hle(const Xbe& xbe) {
    constexpr uint32_t kTitleId = 0x54540079;
    const bool dsound_5849 = std::any_of(xbe.libraries().begin(), xbe.libraries().end(), [](const XbeLibrary& library) {
        return library.name == "DSOUND" && library.build == 5849;
    });
    if (xbe.title_id() != kTitleId || !dsound_5849) {
        log_line("dsound: not the XBE these addresses were taken from; DirectSound is not replaced");
        return;
    }
    for (const Redirect& redirect : kRedirects) {
        auto* code = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(redirect.address));
        code[0] = 0xE9;  // jmp rel32
        const uint32_t relative = address_of(redirect.function) - (redirect.address + 5);
        std::memcpy(code + 1, &relative, sizeof(relative));
    }
    log_line("dsound: %u DirectSound entry points redirected to the XAudio2 mixer",
             static_cast<uint32_t>(sizeof(kRedirects) / sizeof(kRedirects[0])));
}

}  // namespace mc3::pc
