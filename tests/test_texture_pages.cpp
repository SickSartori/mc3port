// A written page is hashed again, the others are not, and the result still follows the content.

#include <cstdint>

#include <vector>

#include "pc/texture_page_hash.h"
#include "test.h"

using namespace mc3::pc;

namespace {

uint32_t g_hashed = 0;

uint64_t sum(const uint8_t* data, uint32_t size) {
    g_hashed += size;
    uint64_t value = 17;
    for (uint32_t i = 0; i < size; ++i) value = value * 31 + data[i];
    return value;
}

uint64_t run(TexturePageHashes& hashes, const std::vector<uint8_t>& memory, uint32_t address, uint32_t checked,
             uint32_t written_page, uint32_t written_generation) {
    g_hashed = 0;
    return hash_written_pages(hashes, memory.data(), address, static_cast<uint32_t>(memory.size()), checked,
                              [=](uint32_t a) { return a / 4096 == written_page ? written_generation : 1u; }, sum);
}

}  // namespace

int main() {
    // Not aligned: 0x10F00 for 10000 bytes touches pages 0x10..0x13, with a short first and last piece.
    constexpr uint32_t kAddress = 0x10F00;
    std::vector<uint8_t> memory(10000, 7);
    TexturePageHashes hashes;

    const uint64_t first = run(hashes, memory, kAddress, 0, 0, 0);
    CHECK(g_hashed == 10000);  // everything the first time
    CHECK(hashes.pages.size() == 4);

    CHECK(run(hashes, memory, kAddress, 5, 0x12, 1) == first);  // nothing newer than the check
    CHECK(g_hashed == 0);

    // A write in the third page: only that piece is read, and the change is seen.
    memory[0x2000] = 8;
    const uint64_t changed = run(hashes, memory, kAddress, 5, 0x12, 6);
    CHECK(g_hashed == 4096);
    CHECK(changed != first);

    // Written back: the same as before, again from that page alone.
    memory[0x2000] = 7;
    CHECK(run(hashes, memory, kAddress, 6, 0x12, 7) == first);
    CHECK(g_hashed == 4096);

    // Another size: all again.
    memory.resize(5000, 7);
    run(hashes, memory, kAddress, 7, 0, 0);
    CHECK(g_hashed == 5000);
    CHECK(hashes.pages.size() == 3);

    // A single page.
    TexturePageHashes small;
    std::vector<uint8_t> one(100, 1);
    run(small, one, 0x20000, 0, 0, 0);
    CHECK(g_hashed == 100);
    CHECK(small.pages.size() == 1);

    return test_result();
}
