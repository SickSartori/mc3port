#include "pc/launcher_music.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <xaudio2.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include "../../res/launcher_resources.h"
#include "pc/log.h"
#include "pc/settings.h"

namespace mc3::pc {

using Microsoft::WRL::ComPtr;

struct LauncherMusic::State {
    std::thread decoder;
    std::atomic<bool> ready{false};
    std::vector<uint8_t> pcm;  // 16-bit stereo
    WAVEFORMATEX format{};
    ComPtr<IXAudio2> engine;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* voice = nullptr;
    float level = 0;
};

namespace {

// Decodes the MP3 resource to 16-bit PCM at its own rate.
bool decode(std::vector<uint8_t>& pcm, WAVEFORMATEX& format) {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&decode), &module);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_LAUNCHER_MUSIC), RT_RCDATA);
    HGLOBAL handle = resource ? LoadResource(module, resource) : nullptr;
    const BYTE* data = handle ? static_cast<const BYTE*>(LockResource(handle)) : nullptr;
    if (!data) return false;
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(data, SizeofResource(module, resource)));
    ComPtr<IMFByteStream> bytes;
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType> wanted, actual;
    if (!stream || FAILED(MFCreateMFByteStreamOnStream(stream.Get(), &bytes)) ||
        FAILED(MFCreateSourceReaderFromByteStream(bytes.Get(), nullptr, &reader)) || FAILED(MFCreateMediaType(&wanted))) {
        return false;
    }
    wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    wanted->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    wanted->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    wanted->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    const DWORD audio = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    if (FAILED(reader->SetCurrentMediaType(audio, nullptr, wanted.Get())) ||
        FAILED(reader->GetCurrentMediaType(audio, &actual))) {
        return false;
    }
    UINT32 rate = 0;
    actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    format = WAVEFORMATEX{WAVE_FORMAT_PCM, 2, rate, rate * 4, 4, 16, 0};
    for (;;) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(audio, 0, nullptr, &flags, nullptr, &sample)) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
        if (!sample) continue;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) break;
        BYTE* samples = nullptr;
        DWORD length = 0;
        if (SUCCEEDED(buffer->Lock(&samples, nullptr, &length))) {
            pcm.insert(pcm.end(), samples, samples + length);
            buffer->Unlock();
        }
    }
    return !pcm.empty() && rate;
}

}  // namespace

LauncherMusic::LauncherMusic() : state_(std::make_unique<State>()) {
    State* state = state_.get();
    state->decoder = std::thread([state] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
            if (decode(state->pcm, state->format)) {
                state->ready = true;
            } else {
                log_line("launcher: the music could not be decoded");
            }
            MFShutdown();
        }
        CoUninitialize();
    });
}

void LauncherMusic::update(float dt, float target) {
    State& s = *state_;
    if (!s.voice && s.ready) {
        s.decoder.join();
        if (FAILED(XAudio2Create(&s.engine, 0, XAUDIO2_DEFAULT_PROCESSOR)) || FAILED(s.engine->CreateMasteringVoice(&s.master)) ||
            FAILED(s.engine->CreateSourceVoice(&s.voice, &s.format))) {
            s.ready = false;
            return;
        }
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes = static_cast<UINT32>(s.pcm.size());
        buffer.pAudioData = s.pcm.data();
        buffer.LoopCount = XAUDIO2_LOOP_INFINITE;
        s.voice->SetVolume(0);
        s.voice->SubmitSourceBuffer(&buffer);
        s.voice->Start();
    }
    if (!s.voice) return;
    s.level += (target - s.level) * std::min(1.0f, dt * 1.5f);
    s.voice->SetVolume(s.level * 0.6f * std::clamp(setting("launcher_music"), 0.0f, 100.0f) / 100.0f);
}

LauncherMusic::~LauncherMusic() {
    State& s = *state_;
    if (s.voice) {
        for (int i = 0; i < 30; ++i) {  // a short fade out
            update(0.1f, 0);
            Sleep(16);
        }
        s.voice->Stop();
        s.voice->DestroyVoice();
    }
    if (s.master) s.master->DestroyVoice();
    s.engine.Reset();
    if (s.decoder.joinable()) s.decoder.join();
}

}  // namespace mc3::pc
