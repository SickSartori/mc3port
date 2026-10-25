#include "pc/physical_memory.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "pc/log.h"

namespace mc3::pc {
namespace {

constexpr uint32_t kPageSize = 4096;
constexpr uint32_t kPageCount = kPhysicalMemorySize / kPageSize;
constexpr uint32_t kKernelReservedSize = 0x00100000;  // the Xbox kernel image and its data live at the bottom

std::mutex g_mutex;
std::vector<bool> g_used(kPageCount, false);
std::unordered_map<uint32_t, uint32_t> g_allocations;  // virtual address -> size in bytes

void mark(uint32_t first_page, uint32_t page_count, bool used) {
    for (uint32_t page = first_page; page < first_page + page_count; ++page) g_used[page] = used;
}

std::mutex g_write_mutex;
std::vector<uint32_t> g_page_generation(kPageCount, 1);
uint32_t g_generation = 1;
std::vector<void*> g_written_pages(kPageCount);

void mark_pages(uint32_t offset, uint32_t bytes, uint32_t generation) {
    if (!bytes || offset >= kPhysicalMemorySize) return;
    const uint32_t last = std::min(offset + bytes - 1, kPhysicalMemorySize - 1) / kPageSize;
    for (uint32_t page = offset / kPageSize; page <= last; ++page) g_page_generation[page] = generation;
}

}  // namespace

uint32_t refresh_physical_writes() {
    std::lock_guard<std::mutex> lock(g_write_mutex);
    const uint32_t generation = ++g_generation;
    for (;;) {
        ULONG_PTR count = g_written_pages.size();
        DWORD granularity = 0;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, reinterpret_cast<void*>(static_cast<uintptr_t>(kPhysicalMapBase)),
                          kPhysicalMemorySize, g_written_pages.data(), &count, &granularity) != 0) {
            break;
        }
        for (ULONG_PTR i = 0; i < count; ++i) {
            const uint32_t offset = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_written_pages[i])) - kPhysicalMapBase;
            g_page_generation[offset / kPageSize] = generation;
        }
        if (count < g_written_pages.size()) break;
    }
    return generation;
}

uint32_t physical_write_generation(uint32_t physical_address, uint32_t bytes) {
    physical_address &= 0x7FFFFFFF;
    if (!bytes || physical_address >= kPhysicalMemorySize) return 0;
    const uint32_t last = std::min(physical_address + bytes - 1, kPhysicalMemorySize - 1) / kPageSize;
    std::lock_guard<std::mutex> lock(g_write_mutex);
    uint32_t latest = 0;
    for (uint32_t page = physical_address / kPageSize; page <= last; ++page) {
        latest = std::max(latest, g_page_generation[page]);
    }
    return latest;
}

void mark_physical_written(uint32_t virtual_address, uint32_t bytes) {
    if (!is_physical_window(virtual_address)) return;
    std::lock_guard<std::mutex> lock(g_write_mutex);
    mark_pages(virtual_address - kPhysicalMapBase, bytes, g_generation + 1);  // seen by the next refresh
}

bool map_physical_memory() {
    void* base = VirtualAlloc(reinterpret_cast<void*>(static_cast<uintptr_t>(kPhysicalMapBase)), kPhysicalMemorySize,
                              MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_EXECUTE_READWRITE);
    if (!base) {
        log_line("cannot map Xbox physical memory at %08X (error %lu)", kPhysicalMapBase, GetLastError());
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    mark(0, kKernelReservedSize / kPageSize, true);
    mark(kInstanceMemoryPhysical / kPageSize, kInstanceMemorySize / kPageSize, true);
    return true;
}

uint32_t allocate_contiguous(uint32_t size, uint32_t lowest, uint32_t highest, uint32_t alignment) {
    if (size == 0) return 0;
    const uint32_t pages = (size + kPageSize - 1) / kPageSize;
    const uint32_t align_pages = alignment > kPageSize ? (alignment + kPageSize - 1) / kPageSize : 1;
    const uint32_t lowest_page = (lowest + kPageSize - 1) / kPageSize;
    const uint32_t highest_page = std::min<uint64_t>(static_cast<uint64_t>(highest) / kPageSize, kPageCount - 1);
    if (highest_page + 1 < pages) return 0;

    std::lock_guard<std::mutex> lock(g_mutex);
    // Top-down like the Xbox kernel, which keeps low memory for the title image.
    for (int64_t first = static_cast<int64_t>(highest_page) + 1 - pages; first >= lowest_page; --first) {
        if (first % align_pages) {
            first -= first % align_pages;
            if (first < lowest_page) break;
        }
        bool free = true;
        for (uint32_t page = static_cast<uint32_t>(first); page < first + pages; ++page) {
            if (g_used[page]) {
                free = false;
                break;
            }
        }
        if (!free) continue;
        mark(static_cast<uint32_t>(first), pages, true);
        const uint32_t address = kPhysicalMapBase + static_cast<uint32_t>(first) * kPageSize;
        g_allocations[address] = size;
        std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(address)), 0, pages * kPageSize);
        return address;
    }
    log_line("allocate_contiguous: no room for %08X bytes in %08X-%08X (align %X)", size, lowest, highest, alignment);
    return 0;
}

void free_contiguous(uint32_t virtual_address) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_allocations.find(virtual_address);
    if (it == g_allocations.end()) return;
    mark((virtual_address - kPhysicalMapBase) / kPageSize, (it->second + kPageSize - 1) / kPageSize, false);
    g_allocations.erase(it);
}

uint32_t contiguous_allocation_size(uint32_t virtual_address) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_allocations.find(virtual_address);
    return it == g_allocations.end() ? 0 : it->second;
}

bool is_physical_window(uint32_t virtual_address) {
    return virtual_address >= kPhysicalMapBase && virtual_address - kPhysicalMapBase < kPhysicalMemorySize;
}

uint32_t physical_to_virtual(uint32_t physical_address) { return kPhysicalMapBase + physical_address; }

uint8_t* physical_pointer(uint32_t physical_address) {
    physical_address &= 0x7FFFFFFF;  // also accept addresses already in the 0x80000000 window
    if (physical_address >= kPhysicalMemorySize) return nullptr;
    return reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(kPhysicalMapBase + physical_address));
}

}  // namespace mc3::pc
