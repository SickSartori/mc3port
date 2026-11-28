#include "pc/hle_patches.h"

#include <windows.h>

#include <cstdint>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

#include "pc/exec_memory.h"
#include "pc/frame_step.h"
#include "pc/game_pause.h"
#include "pc/log.h"
#include "pc/nv2a.h"
#include "pc/renderer_d3d11.h"
#include "pc/settings.h"

namespace xport::pc {
namespace {

// --- picture shape and field of view ---
//
// The game's video modes (0x4B4D80, 16 bytes each: size, flags, shape) give its 720x480 widescreen modes 16/9; the
// camera's perspective (0x2D4AE0) keeps the vertical angle and divides by that shape, so a wider value widens the view
// to the sides. Setting widescreen 2 puts the screen's shape there (renderer_aspect). The same function turns the
// vertical angle (degrees) into half of it in radians with fmul [0x476EBC] (pi / 360, read nowhere else): that
// instruction reads g_half_degree instead, scaled by setting field_of_view.
constexpr uint32_t kWideModeShapes[] = {0x004B4D98, 0x004B4DB8};
constexpr uint32_t kSixteenNinths = 0x3FE38E39;  // 16/9 as the game stores it
constexpr uint32_t kHalfDegreeSite = 0x002D4B24;   // fmul dword ptr [0x476EBC]
constexpr float kHalfDegree = 0.0087266462f;        // pi / 360
volatile float g_half_degree = kHalfDegree;

void scale_field_of_view() {
    g_half_degree = kHalfDegree * std::clamp(setting("field_of_view"), 50.0f, 150.0f) / 100;
}

void patch_shape_and_field_of_view() {
    const float shape = renderer_aspect();
    for (const uint32_t address : kWideModeShapes) {
        auto* value = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(address));
        if (*value != kSixteenNinths) {
            log_line("HLE: video mode shape at %08X does not match this XBE, not changed", address);
        } else if (shape > 16.0f / 9.0f) {
            std::memcpy(value, &shape, sizeof(shape));
            log_line("HLE: video mode shape at %08X set to %.3f", address, shape);
        }
    }
    auto* code = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(kHalfDegreeSite));
    static const uint8_t kFmul[] = {0xD8, 0x0D, 0xBC, 0x6E, 0x47, 0x00};
    if (std::memcmp(code, kFmul, sizeof(kFmul)) != 0) {
        log_line("HLE: field of view at %08X does not match this XBE, not patched", kHalfDegreeSite);
        return;
    }
    const auto ours = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_half_degree));
    std::memcpy(code + 2, &ours, sizeof(ours));
    scale_field_of_view();
    log_line("HLE: field of view at %08X reads the port's copy", kHalfDegreeSite);
}

// --- time stamp counter ---
//
// The game turns the CPU time stamp counter into time assuming the Xbox's 733 MHz: its millisecond clock (0x1A1C00)
// divides rdtsc by 733333 and XAPI's QueryPerformanceFrequency returns 733333333. The PC's counter runs at several
// GHz, so everything timed that way went 5-6 times too fast (menu key repeat made the controller "too sensitive").
// These rdtsc sites read a 733 MHz counter made from the game's performance counter instead (game_ticks: from the
// game's start, without the time it spent paused in the in-game menu).
//
// From the start: counted from Windows' start, after days of uptime the menus' float clock ([0x534674]+0x160, read
// by mcFeMemCard) began at hundreds of thousands of seconds, where a 1/60 s step is under half a float step and is
// lost: the clock stood still and "Checking saved games" waited for ever for its 0.5 s (issue #12; at 30 fps the
// 1/30 s step rounded up and it passed).

constexpr uint64_t kXboxCpuHz = 733333333;

uint64_t xbox_time_stamp() {
    static const uint64_t frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return static_cast<uint64_t>(value.QuadPart);
    }();
    const auto counter = static_cast<uint64_t>(game_ticks());
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
// The shortest step and the unit steps are whole multiples of: 1/60 s as on the Xbox, 1/120 s at 120 fps
// (apply_hle_patches sets it before the game runs).
float g_step_unit = 1.0f / 60;
constexpr float kMaximumCarry = 1.0f / 20;

// Steps a second at the shortest step: 60, or 120 with frame_rate 120.
float step_rate() { return nv2a_vblank_hz() >= 240 ? 120.0f : 60.0f; }
float g_step_carry = 0;

// Replaces the round-up (0x20B737, measured seconds in xmm0); stores the step and continues at 0x20B776 like it.
__declspec(naked) void quantize_frame_step() {
    __asm {
        addss xmm0, g_step_carry  // wanted = measured + carry
        movss xmm1, xmm0
        divss xmm1, g_step_unit
        cvtss2si eax, xmm1        // nearest whole sixtieths, at least one
        cmp eax, 1
        jge whole
        mov eax, 1
    whole:
        cvtsi2ss xmm1, eax
        mulss xmm1, g_step_unit
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

// --- GPU channel ---
//
// Direct3D's kick-off (0x4163C0) flushes the write-back cache and writes PUT about 2,400 times a second, and a helper
// (0x416260) reads GET about 1,300 times: 11,000 of the game's 14,000 register traps a second, half of its core's time
// in the kernel. The flush does nothing on a PC (caches are coherent), so it goes, and PUT and GET are reached by a
// call. [device+0x1C20] can also point at a channel in memory (0x416440): that one is accessed as before.

// Replaces 0x4163E7-0x41641A: PUT in edx, device in esi; continues at 0x41641B with ecx and edx as the original left them.
__declspec(naked) void kick_off_without_trap() {
    __asm {
        mov ecx, dword ptr [esi + 0x1C20]
        and edx, 0x0FFFFFFF
        cmp ecx, kNv2aUserChannel
        jne in_memory
        push ecx
        push edx
        push edx
        call nv2a_kick_off
        add esp, 4
        pop edx
        pop ecx
        jmp done
    in_memory:
        mov dword ptr [ecx + 0x40], edx
    done:
        push 0x0041641B
        ret
    }
}

// Replaces 0x416260-0x416268: device in ecx, GET wanted in eax; edx is not live.
__declspec(naked) void read_get_without_trap() {
    __asm {
        mov eax, dword ptr [ecx + 0x1C20]
        cmp eax, kNv2aUserChannel
        jne in_memory
        push ecx
        call nv2a_dma_get
        pop ecx
        jmp done
    in_memory:
        mov eax, dword ptr [eax + 0x44]
    done:
        push 0x00416269
        ret
    }
}

// The WMA music decoder's overlap-add (WMADEC, 0x42CDCC and 0x42D12C) runs its loop of 16 samples as a do-while: with
// fewer than 16 samples left (the rest goes to the code after it) the count is 0, the first "dec eax" makes it -1,
// and the loop ran on for 4 billion turns, reading and writing past its buffers until a page was missing: the crash
// at a song change (issue #13; at 0x42D351 eax was -2231). On the Xbox it evidently never came to that; here, with
// the port's timing, it did. A count of 0 now goes straight past the loop, its registers as they were.
// D3D's fence wait (0x4165A0), when the GPU is still far from the fence (8 KB of commands or more), turns the NOPs
// reserved at the fence into a software method that sets an event (device+0x1DCC) and sleeps on it (0x4166E8). Far is
// measured from GET, which here follows the drawing; with the render thread the GPU thread has read well past it, so
// the NOPs could already be read as NOPs: the event never came and the game froze (switching cars in the garage at
// 120 fps, report 159174). The wait now also ends when the fence is passed, as the spin for a near fence does (signed,
// like the patched loop at 0x4166DD).
// The game's own KeWaitForSingleObject (in edi at the hook) waits on the event as before, so its set wakes the game
// at once; a timeout of 1 ms looks at the fence. (Polling with Sleep(1) cost frames: up to 15 ms a wait.)
using KeWaitForSingleObjectFunction = int32_t(__stdcall*)(uint32_t object, uint32_t reason, uint32_t mode,
                                                          uint32_t alertable, int64_t* timeout);
void __cdecl wait_fence_or_event(uint32_t device, uint32_t fence, uint32_t wait_function) {
    const auto wait = reinterpret_cast<KeWaitForSingleObjectFunction>(static_cast<uintptr_t>(wait_function));
    const auto issued = reinterpret_cast<volatile uint32_t*>(device + 0x2C);
    const auto* done = reinterpret_cast<volatile uint32_t*>(*reinterpret_cast<volatile uint32_t*>(device + 0x30));
    for (;;) {
        int64_t timeout = -10000;  // 1 ms, relative, in 100 ns
        if (wait(device + 0x1DCC, 6, 1, 0, &timeout) == 0) return;  // Executive, UserMode, as the game's own call
        if (static_cast<int32_t>(*issued - fence) >= static_cast<int32_t>(*issued - *done)) return;
    }
}

// Replaces "add esi,1DCCh" at 0x4166EE (esi = device, edi = KeWaitForSingleObject just loaded, ebp = the fence's record, whose first word the spin at 0x4166CD
// waits for when it comes from the same place) and returns for the function, as its own epilogue at 0x416703 does: pop
// ebx, ebp, edi, esi; ret 8.
__declspec(naked) void d3d_fence_event_wait() {
    __asm {
        push edi
        push dword ptr [ebp]
        push esi
        call wait_fence_or_event
        add esp, 12
        pop ebx
        pop ebp
        pop edi
        pop esi
        ret 8
    }
}

__declspec(naked) void wma_overlap_first() {
    __asm {
        movd mm0, dword ptr [ebp - 1Ch]
        movd mm1, dword ptr [ebp - 24h]
        mov eax, dword ptr [ebp - 30h]
        test eax, eax
        jz none
        push 0042CDCCh
        ret
    none:
        push 0042D0A8h
        ret
    }
}
__declspec(naked) void wma_overlap_second() {
    __asm {
        movd mm0, dword ptr [ebp - 1Ch]
        movd mm1, dword ptr [ebp - 24h]
        mov eax, dword ptr [ebp - 40h]
        test eax, eax
        jz none
        push 0042D12Ch
        ret
    none:
        push 0042D428h
        ret
    }
}

// MC3_TRACE_UNLOCK=1: what the game asks its "is it unlocked?" (0x292CD0, thiscall (type, id, default), ret 0Ch,
// answer in AL; sick-xport analisi-0.5.0/E-sblocca-xbe.md), for an "unlock all" of our own: each (type, id, caller) in
// the log once, and again when its answer changes, with how deep in the progress cache's recount (0x292D70) it came.
// Nothing changes for the game.
volatile int g_unlock_cache_depth = 0;  // the game's code runs on one core

void __stdcall trace_unlock(uint32_t type, uint32_t id, uint32_t fallback, uint32_t from, uint32_t result) {
    static std::mutex mutex;
    static std::map<std::tuple<uint32_t, uint32_t, uint32_t>, uint32_t> seen;
    std::lock_guard<std::mutex> lock(mutex);
    const auto [it, added] = seen.try_emplace({type, id, from}, result);
    if (!added && it->second == result) return;
    it->second = result;
    log_line("unlock: type %u id %08X default %u from %08X cache %d -> %u", type, id, fallback, from,
             g_unlock_cache_depth, result);
}

__declspec(naked) void unlock_query_original() {  // the 7 bytes the jump covers, then the rest of it
    __asm {
        sub esp, 14h
        mov edx, dword ptr [esp + 20h]
        push 0x00292CD7
        ret
    }
}
__declspec(naked) void unlock_query_traced() {
    __asm {
        push ecx                         // this
        push dword ptr [esp + 16]        // default
        push dword ptr [esp + 16]        // id
        push dword ptr [esp + 16]        // type
        mov ecx, dword ptr [esp + 12]
        call unlock_query_original       // ret 0Ch
        movzx eax, al
        push eax                         // kept
        push eax                         // result
        push dword ptr [esp + 12]        // caller
        push dword ptr [esp + 28]        // default
        push dword ptr [esp + 28]        // id
        push dword ptr [esp + 28]        // type
        call trace_unlock
        pop eax
        pop ecx
        ret 0Ch
    }
}
__declspec(naked) void unlock_cache_original() {  // the 5 bytes the jump covers
    __asm {
        push ebp
        mov ebp, dword ptr [esp + 8]
        push 0x00292D75
        ret
    }
}
__declspec(naked) void unlock_cache_traced() {  // thiscall (list, notify), ret 8; ecx untouched
    __asm {
        inc g_unlock_cache_depth
        push dword ptr [esp + 8]
        push dword ptr [esp + 8]
        call unlock_cache_original
        dec g_unlock_cache_depth
        ret 8
    }
}

struct JumpPatch {
    const char* name;
    uint32_t address;
    std::vector<uint8_t> original;  // checked before patching
    const void* target;
};

const JumpPatch kJumpPatches[] = {
    {"D3D fence wait, the event", 0x004166EE, {0x81, 0xC6, 0xCC, 0x1D, 0x00, 0x00},
     reinterpret_cast<const void*>(&d3d_fence_event_wait)},
    {"WMA overlap-add, first loop", 0x0042CDC1, {0x0F, 0x6E, 0x45, 0xE4, 0x0F, 0x6E, 0x4D, 0xDC, 0x8B, 0x45, 0xD0},
     reinterpret_cast<const void*>(&wma_overlap_first)},
    {"WMA overlap-add, second loop", 0x0042D121, {0x0F, 0x6E, 0x45, 0xE4, 0x0F, 0x6E, 0x4D, 0xDC, 0x8B, 0x45, 0xC0},
     reinterpret_cast<const void*>(&wma_overlap_second)},
    {"game rdtsc", 0x001A1BF0, {0x0F, 0x31, 0xC3, 0xCC, 0xCC}, reinterpret_cast<const void*>(&rdtsc_function)},
    {"game rdtsc (low half)", 0x0020E140, {0x52, 0x0F, 0x31, 0x5A, 0xC3},
     reinterpret_cast<const void*>(&rdtsc_low_function)},
    {"XAPI QueryPerformanceCounter", 0x0032633B, {0x8B, 0x4C, 0x24, 0x04, 0x0F},
     reinterpret_cast<const void*>(&query_performance_counter)},
    {"D3D rdtsc", 0x0041B040, {0x0F, 0x31, 0xC3, 0xCC, 0xCC}, reinterpret_cast<const void*>(&rdtsc_function)},
    {"time manager fixed step", 0x0020BB00, {0x8B, 0x44, 0x24, 0x08, 0x85}, reinterpret_cast<const void*>(&set_fixed_step)},
    {"time manager round-up", 0x0020B737, {0xF3, 0x0F, 0x10, 0x0D, 0xF0},
     reinterpret_cast<const void*>(&quantize_frame_step)},
    {"D3D kick-off", 0x004163E7, {0x8B, 0x88, 0x10, 0x04, 0x10}, reinterpret_cast<const void*>(&kick_off_without_trap)},
    {"D3D read of GET", 0x00416260, {0x8B, 0x81, 0x20, 0x1C, 0x00}, reinterpret_cast<const void*>(&read_get_without_trap)},
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
    // XACT, having read a stream's last data, asks (0x40603C) whether a read is still on its way: on the Xbox it is. Here
    // reads finish at once, so a sound that fits in one read (the pedestrians' voices) had its data dropped unplayed
    // and the stream closed. Streams without a decoder ([esi+4Ch] == 0) skip the question; the WMA music keeps the
    // original outcome (no read in flight: clear bit 3), else its decoder overran its buffer at a song change.
    // D3D's wait for a fence (0x4165A0) compares "handed to the GPU minus fence" with "handed minus done" unsigned, the
    // first read once. When another thread hands more work and the GPU passes the first reading meanwhile, "done" is
    // past "handed", the difference wraps and the last loop spins forever: on the Xbox the window is microseconds,
    // with fast frames here it froze the game at "Checking saved games". Signed: jge and jl instead of jae and jb.
    {"D3D fence wait, first check", 0x004165BC, {0x3B, 0xCA, 0x0F, 0x83, 0x1F, 0x01, 0x00, 0x00}, {0x3B, 0xCA, 0x0F, 0x8D}},
    {"D3D fence wait, loop", 0x004166DD, {0x3B, 0xC7, 0x72, 0xF6}, {0x3B, 0xC7, 0x7C}},
    // D3D's vertical blank work (0x41B170) retires a queued flip only when its count of blanks equals the flip's
    // target (cmp count, target; jne). The count moves on the game's own poll of the interrupt registers as well as on
    // the interrupt, two threads here where the Xbox had one, so a blank can be counted twice and skip the target: the
    // flip then waits for the count to come round. Retire at or past the target (jl: not yet while count < target).
    {"D3D flip retirement at or past the target", 0x0041B1B4, {0x75, 0x7F, 0xC7, 0x01, 0x00, 0x00}, {0x7C, 0x7F}},
    {"XACT reads in flight", 0x0040B5B0,
     {0xFF, 0x76, 0x40, 0x8B, 0x0D, 0xA8, 0x0F, 0x41, 0x00, 0xE8, 0x7E, 0xAA, 0xFF, 0xFF, 0x85, 0xC0, 0x75, 0x03, 0x80, 0x26, 0xF7},
     {0x83, 0x7E, 0x4C, 0x00, 0x74, 0x0F, 0x80, 0x26, 0xF7, 0xEB, 0x0A}},  // cmp; je 40B5C5; and; jmp 40B5C5
};

}  // namespace

// On the GPU thread, for each new frame shown: `vblanks` since the previous one. The average over the last frames sets
// the fixed step, so that game time follows real time even when frames come late now and then.
void time_frame_shown(uint32_t vblanks) {
    // A frame of 100 ms or more (MC3_LONG_FRAME_MS=<ms> sets another limit) is a hitch the player sees: in the log, to
    // find what made it.
    static const uint32_t long_frame_ms = [] {
        char value[16] = {};
        return GetEnvironmentVariableA("MC3_LONG_FRAME_MS", value, sizeof(value)) ? std::max(1, std::atoi(value)) : 100;
    }();
    if (vblanks * 1000 >= long_frame_ms * nv2a_vblank_hz() && vblanks < nv2a_vblank_hz() * 10) {
        log_line("time: a frame took %u ms", vblanks * 1000 / nv2a_vblank_hz());
    }
    static uint32_t recent[8] = {};
    static uint32_t shown = 0;
    scale_field_of_view();  // the in-game menu changes it while playing
    recent[shown++ % 8] = std::min<uint32_t>(vblanks, 8);
    if (shown < 8) return;
    uint32_t total = 0;
    for (const uint32_t v : recent) total += v;
    // Whole sixtieths, at least one (1/60 s, the shortest step the game had on the Xbox), changed only when the pace
    // clearly changes (step_sixtieths): where frames jitter between 1 and 3 blanks the average wandered between 0.0156
    // and 0.0229 s every frame. (The
    // menus that stood still at 60 fps, issue #12, were a float clock grown too large: see game_ticks.)
    // MC3_STEP_FLOOR=<seconds> raises the result (diagnostic).
    static const float floor = [] {
        char value[32] = {};
        return GetEnvironmentVariableA("MC3_STEP_FLOOR", value, sizeof(value)) ? static_cast<float>(std::atof(value)) : 0.0f;
    }();
    const float average = static_cast<float>(total) / 8.0f / static_cast<float>(nv2a_vblank_hz());
    static float sixtieths = 0;
    sixtieths = step_sixtieths(sixtieths, average * step_rate());  // in 1/120 s at 120 fps
    const float period = std::max(sixtieths / step_rate(), floor);
    g_frame_period = period;
    if (*g_fixed_mode == 1) use_fixed_step(period);
}
bool g_game_loaded = false;  // the XBE is in place: its variables can be read

int32_t* career_money() {
    if (!g_game_loaded) return nullptr;
    // The career's money: what the game's codes rimbuk and kubmir change by one (0x001924F7),
    // [[0x00557F0C] + 0xC] + 0xACC.
    const auto readable = [](uint32_t address) {
        MEMORY_BASIC_INFORMATION info{};
        return address >= 0x10000 &&
               VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(address)), &info, sizeof(info)) &&
               info.State == MEM_COMMIT;
    };
    const uint32_t game = *reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(0x00557F0C));
    if (!readable(game + 0xC)) return nullptr;
    const uint32_t player = *reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(game + 0xC));
    if (!readable(player + 0xACC)) return nullptr;
    return reinterpret_cast<int32_t*>(static_cast<uintptr_t>(player + 0xACC));
}

void apply_hle_patches(const Xbe& /*xbe*/) {
    g_game_loaded = true;
    g_step_unit = 1.0f / step_rate();
    for (const Patch& patch : kPatches) {
        auto* code = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(patch.address));
        if (std::memcmp(code, patch.original.data(), patch.original.size()) != 0) {
            log_line("HLE: %s at %08X does not match this XBE, not patched", patch.name, patch.address);
            continue;
        }
        std::memcpy(code, patch.replacement.data(), patch.replacement.size());
        log_line("HLE: %s at %08X replaced", patch.name, patch.address);
    }
    const auto redirect = [](const JumpPatch& patch) {
        auto* code = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(patch.address));
        if (std::memcmp(code, patch.original.data(), patch.original.size()) != 0) {
            log_line("HLE: %s at %08X does not match this XBE, not patched", patch.name, patch.address);
            return;
        }
        code[0] = 0xE9;  // jmp rel32
        const uint32_t relative = address_of(patch.target) - (patch.address + 5);
        std::memcpy(code + 1, &relative, sizeof(relative));
        log_line("HLE: %s at %08X redirected", patch.name, patch.address);
    };
    for (const JumpPatch& patch : kJumpPatches) redirect(patch);
    if (GetEnvironmentVariableA("MC3_TRACE_UNLOCK", nullptr, 0)) {
        redirect({"unlock query (traced)", 0x00292CD0, {0x83, 0xEC, 0x14, 0x8B, 0x54, 0x24, 0x20},
                  reinterpret_cast<const void*>(&unlock_query_traced)});
        redirect({"unlock cache recount (traced)", 0x00292D70, {0x55, 0x8B, 0x6C, 0x24, 0x08},
                  reinterpret_cast<const void*>(&unlock_cache_traced)});
    }
    patch_shape_and_field_of_view();
}

}  // namespace xport::pc
