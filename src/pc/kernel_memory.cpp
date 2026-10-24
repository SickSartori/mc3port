// Virtual, contiguous and pool memory. Xbox "physical" memory is plain process memory here: the GPU and audio
// hardware that would read it physically are replaced by the port.

#include <windows.h>

#include <algorithm>
#include <mutex>
#include <unordered_map>

#include "pc/kernel_internal.h"
#include "pc/log.h"

namespace mc3::pc {
namespace {

constexpr uint32_t kXboxPageNoCache = 0x200;
constexpr uint32_t kXboxPageWriteCombine = 0x400;
constexpr uint32_t kXboxMemNoZero = 0x800000;
constexpr uint32_t kXboxPhysicalPages = 64 * 1024 * 1024 / 4096;

std::mutex g_mutex;
std::unordered_map<uint32_t, uint32_t> g_contiguous_sizes;

DWORD windows_protection(uint32_t xbox_protection) {
    const DWORD protection = xbox_protection & ~(kXboxPageNoCache | kXboxPageWriteCombine);
    return protection ? protection : PAGE_READWRITE;
}

NTSTATUS status_from_last_error() {
    switch (GetLastError()) {
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_COMMITMENT_LIMIT:
            return kStatusNoMemory;
        default:
            return kStatusInvalidParameter;
    }
}

NTSTATUS __stdcall NtAllocateVirtualMemory(uint32_t* base_address, uint32_t /*zero_bits*/, uint32_t* region_size,
                                           uint32_t allocation_type, uint32_t protect) {
    void* result = VirtualAlloc(reinterpret_cast<void*>(static_cast<uintptr_t>(*base_address)), *region_size,
                                allocation_type & ~kXboxMemNoZero, windows_protection(protect));
    if (!result) {
        log_line("NtAllocateVirtualMemory(%08X, %08X, type %08X, protect %08X) failed: error %lu", *base_address,
                 *region_size, allocation_type, protect, GetLastError());
        return status_from_last_error();
    }
    MEMORY_BASIC_INFORMATION info;
    VirtualQuery(result, &info, sizeof(info));
    *base_address = address_of(result);
    *region_size = static_cast<uint32_t>(info.RegionSize);
    return kStatusSuccess;
}

NTSTATUS __stdcall NtFreeVirtualMemory(uint32_t* base_address, uint32_t* region_size, uint32_t free_type) {
    void* address = reinterpret_cast<void*>(static_cast<uintptr_t>(*base_address));
    const SIZE_T size = (free_type & MEM_RELEASE) ? 0 : *region_size;
    if (!VirtualFree(address, size, free_type)) return status_from_last_error();
    return kStatusSuccess;
}

NTSTATUS __stdcall NtQueryVirtualMemory(uint32_t base_address, MEMORY_BASIC_INFORMATION* buffer) {
    if (!VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(base_address)), buffer, sizeof(*buffer))) {
        return kStatusInvalidParameter;
    }
    return kStatusSuccess;
}

uint32_t __stdcall MmAllocateContiguousMemoryEx(uint32_t number_of_bytes, uint32_t /*lowest*/, uint32_t /*highest*/,
                                                uint32_t alignment, uint32_t protect) {
    if (alignment > 0x10000) log_line("MmAllocateContiguousMemoryEx: alignment %08X is not honoured", alignment);
    void* memory = VirtualAlloc(nullptr, number_of_bytes, MEM_RESERVE | MEM_COMMIT, windows_protection(protect));
    if (!memory) return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_contiguous_sizes[address_of(memory)] = number_of_bytes;
    return address_of(memory);
}

uint32_t __stdcall MmAllocateContiguousMemory(uint32_t number_of_bytes) {
    return MmAllocateContiguousMemoryEx(number_of_bytes, 0, 0xFFFFFFFF, 0, PAGE_READWRITE);
}

void __stdcall MmFreeContiguousMemory(uint32_t base_address) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_contiguous_sizes.erase(base_address);
    }
    VirtualFree(reinterpret_cast<void*>(static_cast<uintptr_t>(base_address)), 0, MEM_RELEASE);
}

void __stdcall MmPersistContiguousMemory(uint32_t, uint32_t, uint8_t) {}

uint32_t __stdcall MmQueryAllocationSize(uint32_t base_address) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_contiguous_sizes.find(base_address);
        if (it != g_contiguous_sizes.end()) return it->second;
    }
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(base_address)), &info, sizeof(info))) return 0;
    return static_cast<uint32_t>(info.RegionSize);
}

uint32_t __stdcall MmQueryAddressProtect(uint32_t virtual_address) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(virtual_address)), &info, sizeof(info))) return 0;
    return info.State == MEM_COMMIT ? info.Protect : 0;
}

void __stdcall MmSetAddressProtect(uint32_t base_address, uint32_t number_of_bytes, uint32_t new_protect) {
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(static_cast<uintptr_t>(base_address)), number_of_bytes,
                   windows_protection(new_protect), &old);
}

// Physical addresses only matter to the replaced hardware; identity keeps them unique and reversible.
uint32_t __stdcall MmGetPhysicalAddress(uint32_t base_address) { return base_address; }

uint32_t __stdcall MmClaimGpuInstanceMemory(uint32_t number_of_bytes, uint32_t* number_of_padding_bytes) {
    // The real kernel hands the top of physical memory to the GPU and returns the end of that area.
    static uint8_t* instance_memory = nullptr;
    static uint32_t instance_size = 0;
    constexpr uint32_t kInstanceSize = 0x10000;
    if (!instance_memory) {
        instance_size = number_of_bytes != 0xFFFFFFFF ? std::max(number_of_bytes, kInstanceSize) : kInstanceSize;
        instance_memory = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, instance_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    }
    if (number_of_padding_bytes) *number_of_padding_bytes = 0;
    return address_of(instance_memory + instance_size);
}

uint32_t __stdcall MmCreateKernelStack(uint32_t number_of_bytes, uint8_t /*debugger_thread*/) {
    void* memory = VirtualAlloc(nullptr, number_of_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return memory ? address_of(memory) + number_of_bytes : 0;
}

void __stdcall MmDeleteKernelStack(uint32_t /*stack_base*/, uint32_t stack_limit) {
    VirtualFree(reinterpret_cast<void*>(static_cast<uintptr_t>(stack_limit)), 0, MEM_RELEASE);
}

void __stdcall MmLockUnlockBufferPages(uint32_t, uint32_t, uint8_t) {}
void __stdcall MmLockUnlockPhysicalPage(uint32_t, uint8_t) {}

#pragma pack(push, 1)
struct XMmStatistics {
    uint32_t length;
    uint32_t total_physical_pages;
    uint32_t available_pages;
    uint32_t virtual_memory_bytes_committed;
    uint32_t virtual_memory_bytes_reserved;
    uint32_t cache_pages_committed;
    uint32_t pool_pages_committed;
    uint32_t stack_pages_committed;
    uint32_t image_pages_committed;
};
#pragma pack(pop)

NTSTATUS __stdcall MmQueryStatistics(XMmStatistics* statistics) {
    if (!statistics || statistics->length != sizeof(XMmStatistics)) return kStatusInvalidParameter;
    statistics->total_physical_pages = kXboxPhysicalPages;
    statistics->available_pages = kXboxPhysicalPages / 2;
    statistics->virtual_memory_bytes_committed = 32 * 1024 * 1024;
    statistics->virtual_memory_bytes_reserved = 64 * 1024 * 1024;
    statistics->cache_pages_committed = 0;
    statistics->pool_pages_committed = 256;
    statistics->stack_pages_committed = 64;
    statistics->image_pages_committed = 0x57A280 / 4096;
    return kStatusSuccess;
}

uint32_t __stdcall ExAllocatePoolWithTag(uint32_t number_of_bytes, uint32_t /*tag*/) {
    return address_of(HeapAlloc(GetProcessHeap(), 0, number_of_bytes ? number_of_bytes : 1));
}

void __stdcall ExFreePool(uint32_t pool) {
    if (pool) HeapFree(GetProcessHeap(), 0, reinterpret_cast<void*>(static_cast<uintptr_t>(pool)));
}

uint32_t __stdcall ExQueryPoolBlockSize(uint32_t pool) {
    return static_cast<uint32_t>(HeapSize(GetProcessHeap(), 0, reinterpret_cast<void*>(static_cast<uintptr_t>(pool))));
}

}  // namespace

void register_memory_functions() {
    register_kernel(184, &NtAllocateVirtualMemory);
    register_kernel(199, &NtFreeVirtualMemory);
    register_kernel(217, &NtQueryVirtualMemory);
    register_kernel(165, &MmAllocateContiguousMemory);
    register_kernel(166, &MmAllocateContiguousMemoryEx);
    register_kernel(171, &MmFreeContiguousMemory);
    register_kernel(178, &MmPersistContiguousMemory);
    register_kernel(180, &MmQueryAllocationSize);
    register_kernel(179, &MmQueryAddressProtect);
    register_kernel(182, &MmSetAddressProtect);
    register_kernel(173, &MmGetPhysicalAddress);
    register_kernel(168, &MmClaimGpuInstanceMemory);
    register_kernel(169, &MmCreateKernelStack);
    register_kernel(170, &MmDeleteKernelStack);
    register_kernel(175, &MmLockUnlockBufferPages);
    register_kernel(176, &MmLockUnlockPhysicalPage);
    register_kernel(181, &MmQueryStatistics);
    register_kernel(15, &ExAllocatePoolWithTag);
    register_kernel(17, &ExFreePool);
    register_kernel(23, &ExQueryPoolBlockSize);
}

}  // namespace mc3::pc
