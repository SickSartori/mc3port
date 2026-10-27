#include "pc/hle_patches.h"

#include <windows.h>

#include <cstdint>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

#include "pc/exec_memory.h"
#include "pc/game_pause.h"
#include "pc/log.h"
#include "pc/nv2a.h"

namespace mc3::pc {
namespace {

// --- time stamp counter ---
//
// The game turns the CPU time stamp counter into time assuming the Xbox's 733 MHz: its millisecond clock (0x1A1C00)
// divides rdtsc by 733333 and XAPI's QueryPerformanceFrequency returns 733333333. The PC's counter runs at several
// GHz, so everything timed that way went 5-6 times too fast (menu key repeat made the controller "too sensitive").
// These rdtsc sites read a 733 MHz counter made from the host's performance counter instead, without the time the
// game spent paused (the in-game menu).

constexpr uint64_t kXboxCpuHz = 733333333;

uint64_t xbox_time_stamp() {
    static const uint64_t frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return static_cast<uint64_t>(value.QuadPart);
    }();
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    const auto counter = static_cast<uint64_t>(value.QuadPart - game_paused_ticks());
    return counter / frequency * kXboxCpuHz + counter % frequency * kXboxCpuHz / frequency;
}

// Replaces "rdtsc; ret": the result in edx:eax, every other register kept.
__declspec(naked) void rdtsc_function() {
    __asm {
        push ecx
        call xbox_time_stamp
        pop ecx
        ret
    }
}

// Replaces "push edx; rdtsc; pop edx; ret": the low half in eax, edx kept.
__declspec(naked) void rdtsc_low_function() {
    __asm {
        push ecx
        push edx
        call xbox_time_stamp
        pop edx
        pop ecx
        ret
    }
}

// XAPI QueryPerformanceCounter (0x32633B): rdtsc into the LARGE_INTEGER, returns TRUE.
uint32_t __stdcall query_performance_counter(uint64_t* counter) {
    *counter = xbox_time_stamp();
    return 1;
}

// --- frame step ---
//
// The game's time manager (0x20B6F0) gives every frame a step in seconds, read by physics and animation (0x4B2D48).
// Most of the game uses a fixed step, set by 0x20BB00 for the frame rate it had on the Xbox: 1/60 s in menus, 1/30 s in
// races (a frame every second 60 Hz blank). The port's 60 fps doubles the vertical blank, so a race draws twice as
// many frames and ran at double speed. The fixed step is now the time the game's frames really take on screen: the
// average number of blanks between the last shown frames (time_frame_shown), at the port's blank rate.
//
// Elsewhere the measured frame time is rounded *up* to 1/60, 1/30, 1/20 or 1/15 s: exact on the Xbox, whose frames
// came on whole blanks, but here the measurement jitters and a frame a little over 1/60 s became 1/30. It is now
// rounded to the nearest 1/60 s, the error carried to the next frame so that game time follows real time.

float* const g_step = reinterpret_cast<float*>(0x4B2D48);           // this frame's step
float* const g_steps_per_second = reinterpret_cast<float*>(0x4B2D50);
float* const g_fixed_rate = reinterpret_cast<float*>(0x4B2D54);
float* const g_fixed_step = reinterpret_cast<float*>(0x4B2D58);
uint32_t* const g_fixed_count = reinterpret_cast<uint32_t*>(0x4B2D64);
uint32_t* const g_fixed_mode = reinterpret_cast<uint32_t*>(0x534770);

std::atomic<float> g_frame_period{0};  // seconds between the game's shown frames; 0 until measured

void use_fixed_step(float step) {
    *g_fixed_step = step;
    *g_steps_per_second = 1.0f / step;
    static float logged = 0;  // only clear changes (the step follows the frame rate)
    if (step > logged * 1.2f || step < logged / 1.2f) {
        log_line("time: fixed frame step %.4f s", step);
        logged = step;
    }
}

void __cdecl set_fixed_step(float rate, uint32_t count) {
    *g_fixed_rate = rate;
    *g_fixed_count = count;
    *g_fixed_mode = 1;
    const float period = g_frame_period;
    use_fixed_step(period > 0 ? period : 1.0f / (static_cast<float>(count) * rate));
    *g_step = *g_fixed_step;
}
constexpr float kSixtieth = 1.0f / 60;
constexpr float kMaximumCarry = 1.0f / 20;
float g_step_carry = 0;

// Replaces the round-up (0x20B737, measured seconds in xmm0); stores the step and continues at 0x20B776 like it.
__declspec(naked) void quantize_frame_step() {
    __asm {
        addss xmm0, g_step_carry  // wanted = measured + carry
        movss xmm1, xmm0
        divss xmm1, kSixtieth
        cvtss2si eax, xmm1        // nearest whole sixtieths, at least one
        cmp eax, 1
        jge whole
        mov eax, 1
    whole:
        cvtsi2ss xmm1, eax
        mulss xmm1, kSixtieth
        subss xmm0, xmm1          // carry = wanted - step, kept small
        movss xmm2, kMaximumCarry
        minss xmm0, xmm2
        xorps xmm2, xmm2
        subss xmm2, kMaximumCarry
        maxss xmm0, xmm2
        movss g_step_carry, xmm0
        movaps xmm0, xmm1
        mov eax, 0x004B2D70
        movss dword ptr [eax], xmm0
        push 0x0020B776
        ret
    }
}

struct JumpPatch {
    const char* name;
    uint32_t address;
    std::vector<uint8_t> original;  // checked before patching
    const void* target;
};

const JumpPatch kJumpPatches[] = {
    {"game rdtsc", 0x001A1BF0, {0x0F, 0x31, 0xC3, 0xCC, 0xCC}, reinterpret_cast<const void*>(&rdtsc_function)},
    {"game rdtsc (low half)", 0x0020E140, {0x52, 0x0F, 0x31, 0x5A, 0xC3},
     reinterpret_cast<const void*>(&rdtsc_low_function)},
    {"XAPI QueryPerformanceCounter", 0x0032633B, {0x8B, 0x4C, 0x24, 0x04, 0x0F},
     reinterpret_cast<const void*>(&query_performance_counter)},
    {"D3D rdtsc", 0x0041B040, {0x0F, 0x31, 0xC3, 0xCC, 0xCC}, reinterpret_cast<const void*>(&rdtsc_function)},
    {"time manager fixed step", 0x0020BB00, {0x8B, 0x44, 0x24, 0x08, 0x85}, reinterpret_cast<const void*>(&set_fixed_step)},
    {"time manager round-up", 0x0020B737, {0xF3, 0x0F, 0x10, 0x0D, 0xF0},
     reinterpret_cast<const void*>(&quantize_frame_step)},
};

struct Patch {
    const char* name;
    uint32_t address;
    std::vector<uint8_t> original;  // checked before patching
    std::vector<uint8_t> replacement;
};

// XAPI input functions (XPP library, XDK 5849), found from the game's call sequence at 0x84B40:
//   XInitDevices(0, 0); while (XGetDeviceEnumerationStatus() == BUSY); XGetDevices(XDEVICE_TYPE_GAMEPAD); ...
// XInitDevices would start the USB host controller driver; instead nothing is enumerated here, and the controllers
// come from the host through the redirected XGetDevices/XInputOpen/XInputGetState (input_hle.cpp).
const Patch kPatches[] = {
    {"XInitDevices", 0x0043F0E7, {0x55, 0x8D, 0x6C, 0x24, 0x90}, {0xC2, 0x08, 0x00}},       // ret 8
    {"XGetDeviceEnumerationStatus", 0x004411A2, {0x56, 0x33, 0xF6}, {0x33, 0xC0, 0xC3}},   // return IDLE (0)
};

}  // namespace

// On the GPU thread, for each new frame shown: `vblanks` since the previous one. The average over the last frames sets
// the fixed step, so that game time follows real time even when frames come late now and then.
void time_frame_shown(uint32_t vblanks) {
    static uint32_t recent[8] = {};
    static uint32_t shown = 0;
    recent[shown++ % 8] = std::min<uint32_t>(vblanks, 8);
    if (shown < 8) return;
    uint32_t total = 0;
    for (const uint32_t v : recent) total += v;
    const float period = static_cast<float>(total) / 8.0f / static_cast<float>(nv2a_vblank_hz());
    if (period <= 0) return;
    g_frame_period = period;
    if (*g_fixed_mode == 1) use_fixed_step(period);
}
void apply_hle_patches(const Xbe& /*xbe*/) {
    for (const Patch& patch : kPatches) {
        auto* code = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(patch.address));
        if (std::memcmp(code, patch.original.data(), patch.original.size()) != 0) {
            log_line("HLE: %s at %08X does not match this XBE, not patched", patch.name, patch.address);
            continue;
        }
        std::memcpy(code, patch.replacement.data(), patch.replacement.size());
        log_line("HLE: %s at %08X replaced", patch.name, patch.address);
    }
    for (const JumpPatch& patch : kJumpPatches) {
        auto* code = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(patch.address));
        if (std::memcmp(code, patch.original.data(), patch.original.size()) != 0) {
            log_line("HLE: %s at %08X does not match this XBE, not patched", patch.name, patch.address);
            continue;
        }
        code[0] = 0xE9;  // jmp rel32
        const uint32_t relative = address_of(patch.target) - (patch.address + 5);
        std::memcpy(code + 1, &relative, sizeof(relative));
        log_line("HLE: %s at %08X redirected", patch.name, patch.address);
    }
}

}  // namespace mc3::pc
