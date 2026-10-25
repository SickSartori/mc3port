#pragma once

#include <cstdint>

namespace mc3::pc {

// The Xbox's 64 MB of RAM, mapped where the Xbox kernel maps it: virtual 0x80000000 + physical address.
// Contiguous memory (textures, vertex buffers, push buffer, framebuffers) is carved out of it, so the
// physical addresses the game hands to the GPU translate back exactly.
constexpr uint32_t kPhysicalMapBase = 0x80000000;
constexpr uint32_t kPhysicalMemorySize = 64 * 1024 * 1024;

// Instance memory the GPU claims at the top of RAM (MmClaimGpuInstanceMemory).
constexpr uint32_t kInstanceMemoryPhysical = 0x03FE0000;
constexpr uint32_t kInstanceMemorySize = kPhysicalMemorySize - kInstanceMemoryPhysical;

// Maps the window and reserves the pages the Xbox kernel itself occupies. Fatal if the range is taken.
bool map_physical_memory();

// Contiguous allocation inside [lowest, highest] physical, aligned; 0 when nothing fits.
uint32_t allocate_contiguous(uint32_t size, uint32_t lowest, uint32_t highest, uint32_t alignment);
void free_contiguous(uint32_t virtual_address);
uint32_t contiguous_allocation_size(uint32_t virtual_address);  // 0 if not a contiguous allocation

// Write tracking, so the renderer re-reads a texture only when its memory changed. Each page carries the
// generation in which a write to it was last seen; refresh_physical_writes() collects the writes made since the
// previous call (Windows write watch) and returns the new current generation.
uint32_t refresh_physical_writes();
uint32_t physical_write_generation(uint32_t physical_address, uint32_t bytes);  // latest over the range
void mark_physical_written(uint32_t virtual_address, uint32_t bytes);  // writes Windows does not see (kernel I/O)

bool is_physical_window(uint32_t virtual_address);
uint32_t physical_to_virtual(uint32_t physical_address);
uint8_t* physical_pointer(uint32_t physical_address);  // nullptr outside RAM

}  // namespace mc3::pc
