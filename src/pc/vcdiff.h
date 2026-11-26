#pragma once

// VCDIFF (RFC 3284) decoding, for the translation patches (translation.cpp): the default code table, no secondary
// compression, and xdelta3's adler32 field in window headers (what `xdelta3 -e -S none -A` writes).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace xport::pc {

// Reads `size` bytes of the source at `offset` into `out`; false when it cannot.
using VcdiffSource = std::function<bool(uint64_t offset, uint8_t* out, size_t size)>;
// Takes the next `size` bytes of the target; false to stop.
using VcdiffSink = std::function<bool(const uint8_t* data, size_t size)>;

// Decodes `delta` against the source; false with `error` set when the delta is not one this reads, or is damaged.
bool vcdiff_decode(const uint8_t* delta, size_t size, const VcdiffSource& source, const VcdiffSink& sink,
                   std::string* error);

}  // namespace xport::pc
