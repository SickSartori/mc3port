#pragma once

#include <cstddef>
#include <cstdint>

namespace mc3::pc {

// Bump allocator for small generated code (patch helpers, kernel stubs). Never freed: lives as long as the game.
class ExecMemory {
public:
    uint8_t* allocate(size_t size);

private:
    uint8_t* cursor_ = nullptr;
    size_t remaining_ = 0;
};

inline uint32_t address_of(const void* pointer) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pointer));
}

// Writes `call target` (E8 rel32) at `at`, 5 bytes.
void emit_call(uint8_t* at, const void* target);

}  // namespace mc3::pc
