#pragma once

#include <cstdint>
#include <string>

#include "core/xbe.h"
#include "pc/exec_memory.h"

namespace mc3::pc {

struct KernelThunkReport {
    uint32_t implemented = 0;
    uint32_t data = 0;
    uint32_t unimplemented = 0;
};

// Makes `function` (a __stdcall with the Xbox signature) the implementation of a kernel ordinal.
// Must be called before install_kernel_thunks.
void register_kernel_function(uint32_t ordinal, const void* function);

// Registers every implemented kernel function. Must be called before install_kernel_thunks.
void register_all_kernel_functions();

// Resolves the XBE's kernel import table, already mapped in memory, in place. Ordinals without an
// implementation get a stub that logs the call (ordinal, name, caller, first arguments) and stops the process,
// so each run shows exactly which kernel function to write next.
bool install_kernel_thunks(const Xbe& xbe, ExecMemory& memory, KernelThunkReport* report, std::string* error);

}  // namespace mc3::pc
