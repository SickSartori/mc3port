#include "pc/exec_memory.h"

#include <windows.h>

#include <algorithm>

#include "core/byteorder.h"

namespace mc3::pc {
namespace {

constexpr size_t kChunkSize = 64 * 1024;
constexpr size_t kAlignment = 16;

}  // namespace

uint8_t* ExecMemory::allocate(size_t size) {
    size = (size + kAlignment - 1) & ~(kAlignment - 1);
    if (size > remaining_) {
        const size_t chunk = std::max(kChunkSize, size);
        cursor_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, chunk, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        remaining_ = cursor_ ? chunk : 0;
        if (!cursor_) return nullptr;
    }
    uint8_t* result = cursor_;
    cursor_ += size;
    remaining_ -= size;
    return result;
}

void emit_call(uint8_t* at, const void* target) {
    at[0] = 0xE8;
    store_le32(at + 1, address_of(target) - (address_of(at) + 5));
}

}  // namespace mc3::pc
