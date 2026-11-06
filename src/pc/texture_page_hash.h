#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace mc3::pc {

// A write to one page of a large texture (another texture or vertices sharing that page, every frame) must not cost
// a look at the whole texture: each page keeps its hash, and only the written ones are hashed again.
struct TexturePageHashes {
    std::vector<uint64_t> pages;
    uint32_t bytes = 0;
};

// generation_of(address): the write generation of that address's page. hash(data, size): the hash of one piece.
template <class GenerationOf, class Hash>
uint64_t hash_written_pages(TexturePageHashes& hashes, const uint8_t* memory, uint32_t address, uint32_t bytes,
                            uint32_t checked_generation, GenerationOf generation_of, Hash hash) {
    constexpr uint32_t kPage = 4096;
    const uint32_t first = address / kPage;
    const uint32_t count = (address + bytes - 1) / kPage - first + 1;
    const bool fresh = hashes.bytes != bytes || hashes.pages.size() != count;
    if (fresh) {
        hashes.pages.assign(count, 0);
        hashes.bytes = bytes;
    }
    uint64_t all = bytes;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t start = std::max(address, (first + i) * kPage);
        const uint32_t end = std::min(address + bytes, (first + i + 1) * kPage);
        if (fresh || generation_of(start) > checked_generation) hashes.pages[i] = hash(memory + (start - address), end - start);
        all = (all << 13 | all >> 51) ^ hashes.pages[i];
    }
    return all;
}

}  // namespace mc3::pc
