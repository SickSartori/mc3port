// System services: time, firmware/HAL, video encoder, EEPROM settings, debugging, XBE sections, and the Rtl
// routines whose Xbox and NT versions are identical (forwarded to ntdll).

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "pc/kernel_internal.h"
#include "pc/log.h"

namespace mc3::pc {
namespace {

const Xbe* g_xbe = nullptr;

// --- time ---

uint64_t __stdcall KeQueryPerformanceCounter() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<uint64_t>(counter.QuadPart);
}

uint64_t __stdcall KeQueryPerformanceFrequency() {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return static_cast<uint64_t>(frequency.QuadPart);
}

void __stdcall KeQuerySystemTime(int64_t* current_time) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    *current_time = (static_cast<int64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
}

NTSTATUS __stdcall NtSetSystemTime(const int64_t*, int64_t*) { return kStatusSuccess; }  // the host clock wins

void __stdcall KeStallExecutionProcessor(uint32_t microseconds) {
    const uint64_t frequency = KeQueryPerformanceFrequency();
    const uint64_t end = KeQueryPerformanceCounter() + frequency * microseconds / 1000000;
    while (KeQueryPerformanceCounter() < end) YieldProcessor();
}

NTSTATUS __stdcall KeSaveFloatingPointState(void*) { return kStatusSuccess; }
NTSTATUS __stdcall KeRestoreFloatingPointState(void*) { return kStatusSuccess; }

// --- firmware, HAL, debugging ---

// Logs the stack words that look like return addresses into the game's code: a cheap backtrace that works
// whether or not the game's functions keep frame pointers.
void log_game_return_addresses(const void* stack_pointer) {
    const Xbe* xbe = g_xbe;
    if (!xbe) return;
    const auto* stack = static_cast<const uint32_t*>(stack_pointer);
    std::string line;
    int found = 0;
    for (int i = 0; i < 1024 && found < 24; ++i) {
        const uint32_t value = stack[i];
        for (const XbeSection& section : xbe->sections()) {
            if (section.executable() && value > section.virtual_address &&
                value < section.virtual_address + section.virtual_size) {
                char entry[24];
                std::snprintf(entry, sizeof(entry), " %08X", value);
                line += entry;
                ++found;
                break;
            }
        }
    }
    log_line("  return addresses on the stack:%s", line.c_str());
}

[[noreturn]] void quit(const char* reason, uint32_t code, const void* caller) {
    log_line("%s (%08X) called from %08X: the game asked to leave", reason, code, address_of(caller));
    log_game_return_addresses(&code);
    // LAUNCH_DATA_PAGE: header { type, title id, launch path[520], flags }, then the launch data.
    if (const uint32_t page = kernel_launch_data_page()) {
        log_line("  launch data: type %u, title %08X, path '%.520s'", *guest<uint32_t>(page),
                 *guest<uint32_t>(page + 4), guest<const char>(page + 8));
        // LD_LAUNCH_DASHBOARD: reason (0 main menu, 1 error, 2 memory, 3 settings...), context, parameters.
        const auto* data = guest<const uint32_t>(page + 0x400);
        log_line("  launch data: %08X %08X %08X %08X", data[0], data[1], data[2], data[3]);
    }
    ExitProcess(0);
}

void __stdcall HalReturnToFirmware(uint32_t routine) { quit("HalReturnToFirmware", routine, _ReturnAddress()); }
void __stdcall HalInitiateShutdown() { quit("HalInitiateShutdown", 0, _ReturnAddress()); }
uint8_t __stdcall HalIsResetOrShutdownPending() { return 0; }
void __stdcall HalRegisterShutdownNotification(void*, uint8_t) {}

void __stdcall HalReadWritePCISpace(uint32_t, uint32_t, uint32_t, void* buffer, uint32_t length, uint8_t write) {
    if (!write) std::memset(buffer, 0, length);
}

uint32_t __stdcall HalGetInterruptVector(uint32_t bus_interrupt_level, uint8_t* irql) {
    constexpr uint32_t kPrimaryVectorBase = 0x30;
    constexpr uint32_t kProfileLevel = 26;
    if (irql) *irql = static_cast<uint8_t>(kProfileLevel - bus_interrupt_level);
    return kPrimaryVectorBase + bus_interrupt_level;
}

[[noreturn]] void __stdcall KeBugCheck(uint32_t code) {
    log_line("KeBugCheck(%08X)", code);
    TerminateProcess(GetCurrentProcess(), 4);
    for (;;) {
    }
}

uint32_t __cdecl DbgPrint(const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    size_t length = std::strlen(text);
    while (length && (text[length - 1] == '\n' || text[length - 1] == '\r')) text[--length] = 0;
    log_line("DbgPrint: %s", text);
    return kStatusSuccess;
}

// --- video encoder ---

constexpr uint32_t kAvQueryAvCapabilities = 6;
constexpr uint32_t kAvStandardPalI = 0x00000300;
constexpr uint32_t kAvPackScart = 0x00000003;

uint32_t __stdcall AvGetSavedDataAddress() { return 0; }
void __stdcall AvSetSavedDataAddress(uint32_t) {}
NTSTATUS __stdcall AvSetDisplayMode(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return kStatusSuccess; }

void __stdcall AvSendTVEncoderOption(uint32_t, uint32_t option, uint32_t, uint32_t* result) {
    if (result) *result = option == kAvQueryAvCapabilities ? (kAvStandardPalI | kAvPackScart) : 0;
}

// --- EEPROM settings (ExQueryNonVolatileSetting) ---

constexpr uint32_t kRegBinary = 3;
constexpr uint32_t kRegDword = 4;

uint32_t configured_language() {
    // XC_LANGUAGE: 1 English, 2 Japanese, 3 German, 4 French, 5 Spanish, 6 Italian.
    char value[8] = {};
    if (GetEnvironmentVariableA("MC3_LANGUAGE", value, sizeof(value))) {
        const int language = std::atoi(value);
        if (language >= 1 && language <= 6) return static_cast<uint32_t>(language);
    }
    return 6;
}

NTSTATUS __stdcall ExQueryNonVolatileSetting(uint32_t value_index, uint32_t* type, void* value, uint32_t value_length,
                                             uint32_t* result_length) {
    uint32_t dword = 0;
    switch (value_index) {
        case 0x0: dword = 0; break;                       // XC_TIMEZONE_BIAS
        case 0x7: dword = configured_language(); break;   // XC_LANGUAGE
        case 0x8: dword = 0x00000040; break;              // XC_VIDEO_FLAGS: PAL 60 Hz allowed
        case 0x9: dword = 0; break;                       // XC_AUDIO_FLAGS: stereo
        case 0xA: case 0xB: case 0xC: dword = 0; break;   // parental control: off
        case 0x11: dword = 0; break;                      // XC_MISC
        case 0x12: dword = 2; break;                      // XC_DVD_REGION
        case 0x103: dword = 0x00800300; break;            // XC_FACTORY_AV_REGION: PAL-I
        case 0x104: dword = 4; break;                     // XC_FACTORY_GAME_REGION: rest of world (PAL)
        default:
            log_line("ExQueryNonVolatileSetting: unknown setting %08X, returning zeros", value_index);
            if (type) *type = kRegBinary;
            if (value && value_length) std::memset(value, 0, value_length);
            if (result_length) *result_length = value_length;
            return kStatusSuccess;
    }
    if (type) *type = kRegDword;
    if (result_length) *result_length = sizeof(dword);
    if (value_length < sizeof(dword)) return kStatusBufferTooSmall;
    std::memcpy(value, &dword, sizeof(dword));
    return kStatusSuccess;
}

// --- XBE sections loaded on demand ---

#pragma pack(push, 1)
struct XbeSectionHeader {
    uint32_t flags;
    uint32_t virtual_address;
    uint32_t virtual_size;
    uint32_t file_address;
    uint32_t file_size;
    uint32_t section_name;
    int32_t section_reference_count;
    uint32_t head_shared_page_reference_count;
    uint32_t tail_shared_page_reference_count;
    uint8_t sha_hash[20];
};
#pragma pack(pop)

NTSTATUS __stdcall XeLoadSection(XbeSectionHeader* section) {
    if (section->section_reference_count++ == 0 && g_xbe) {
        const std::vector<uint8_t>& file = g_xbe->file();
        auto* destination = guest<uint8_t>(section->virtual_address);
        const uint32_t size = std::min(section->file_size, section->virtual_size);
        std::memset(destination, 0, section->virtual_size);
        if (section->file_address <= file.size() && size <= file.size() - section->file_address) {
            std::memcpy(destination, file.data() + section->file_address, size);
        }
        // TODO: executable sections loaded here also need the fs: patch pass.
        log_line("XeLoadSection: %s at %08X (%u bytes)", guest<const char>(section->section_name),
                 section->virtual_address, section->virtual_size);
    }
    return kStatusSuccess;
}

NTSTATUS __stdcall XeUnloadSection(XbeSectionHeader* section) {
    if (section->section_reference_count > 0) --section->section_reference_count;
    return kStatusSuccess;
}

// --- network PHY: no link, the port is offline ---

NTSTATUS __stdcall PhyInitialize(uint8_t, void*) { return kStatusSuccess; }
uint32_t __stdcall PhyGetLinkState(uint8_t) { return 0; }

void register_ntdll(uint32_t ordinal, const char* name) {
    static const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (const FARPROC function = GetProcAddress(ntdll, name)) {
        register_kernel_function(ordinal, reinterpret_cast<const void*>(function));
    } else {
        log_line("ntdll has no %s; ordinal %u stays unimplemented", name, ordinal);
    }
}

}  // namespace

void set_kernel_xbe(const Xbe* xbe) { g_xbe = xbe; }
const Xbe* kernel_xbe() { return g_xbe; }

void register_system_functions() {
    register_kernel(126, &KeQueryPerformanceCounter);
    register_kernel(127, &KeQueryPerformanceFrequency);
    register_kernel(128, &KeQuerySystemTime);
    register_kernel(228, &NtSetSystemTime);
    register_kernel(151, &KeStallExecutionProcessor);
    register_kernel(142, &KeSaveFloatingPointState);
    register_kernel(139, &KeRestoreFloatingPointState);
    register_kernel(49, &HalReturnToFirmware);
    register_kernel(360, &HalInitiateShutdown);
    register_kernel(358, &HalIsResetOrShutdownPending);
    register_kernel(47, &HalRegisterShutdownNotification);
    register_kernel(46, &HalReadWritePCISpace);
    register_kernel(44, &HalGetInterruptVector);
    register_kernel(95, &KeBugCheck);
    register_kernel(8, &DbgPrint);
    register_kernel(1, &AvGetSavedDataAddress);
    register_kernel(4, &AvSetSavedDataAddress);
    register_kernel(3, &AvSetDisplayMode);
    register_kernel(2, &AvSendTVEncoderOption);
    register_kernel(24, &ExQueryNonVolatileSetting);
    register_kernel(327, &XeLoadSection);
    register_kernel(328, &XeUnloadSection);
    register_kernel(253, &PhyInitialize);
    register_kernel(252, &PhyGetLinkState);

    // Same prototypes and structure layouts on Xbox and 32-bit NT.
    register_ntdll(289, "RtlInitAnsiString");
    register_ntdll(260, "RtlAnsiStringToUnicodeString");
    register_ntdll(308, "RtlUnicodeStringToAnsiString");
    register_ntdll(270, "RtlCompareString");
    register_ntdll(279, "RtlEqualString");
    register_ntdll(269, "RtlCompareMemoryUlong");
    register_ntdll(304, "RtlTimeFieldsToTime");
    register_ntdll(305, "RtlTimeToTimeFields");
    register_ntdll(301, "RtlNtStatusToDosError");
    register_ntdll(302, "RtlRaiseException");
    register_ntdll(312, "RtlUnwind");
}

}  // namespace mc3::pc
