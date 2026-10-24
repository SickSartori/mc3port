#pragma once

#include <cstdint>

namespace mc3 {

// Data exports are variables the game reads in place (KeTickCount, XboxKrnlVersion, ...):
// their thunk must point at real storage, never at code.
enum class KernelExportKind : uint8_t { Function, Data };

struct KernelExport {
    uint16_t ordinal;
    KernelExportKind kind;
    const char* name;
};

// One past the highest retail xboxkrnl.exe ordinal.
constexpr uint32_t kKernelOrdinalLimit = 367;

// nullptr for ordinals that do not exist in the retail kernel.
const KernelExport* find_kernel_export(uint32_t ordinal);

}  // namespace mc3
