#pragma once

#include <windows.h>

#include <cstdint>

namespace mc3::pc {

// Device register access for memory-mapped I/O. Registers live in reserved but uncommitted address space, so
// every access faults; the faulting instruction is decoded and replayed against the device callbacks.
struct MmioDevice {
    uint32_t (*read)(uint32_t address, uint32_t size);
    void (*write)(uint32_t address, uint32_t size, uint32_t value);
};

// Emulates the memory access of the instruction at context->Eip, which faulted on `address`, and steps past it.
// Handles the forms compilers emit for register access: mov/movzx/movsx loads and stores, test, cmp, and or/and
// read-modify-write, with 8/16/32-bit operands. Returns false for anything else.
bool emulate_mmio_access(CONTEXT* context, uint32_t address, const MmioDevice& device);

}  // namespace mc3::pc
