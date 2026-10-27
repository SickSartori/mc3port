#include "pc/hle_patches.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "pc/exec_memory.h"
#include "pc/game_pause.h"
#include "pc/log.h"

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
