#include "pc/vcdiff.h"

#include <array>
#include <cstring>
#include <vector>

namespace xport::pc {
namespace {

enum : uint8_t { kNoop = 0, kAdd = 1, kRun = 2, kCopy = 3 };
constexpr int kNear = 4, kSame = 3;  // the default address cache (RFC 3284, 5.1)

struct Instruction {
    uint8_t type = kNoop, size = 0, mode = 0;
};
using CodeTable = std::array<std::array<Instruction, 2>, 256>;

// The default code table (RFC 3284, 5.6).
const CodeTable& default_table() {
    static const CodeTable table = [] {
        CodeTable t{};
        int i = 0;
        t[i++][0] = {kRun, 0, 0};
        for (int size = 0; size <= 17; ++size) t[i++][0] = {kAdd, static_cast<uint8_t>(size), 0};
        for (int mode = 0; mode <= 8; ++mode) {
            t[i++][0] = {kCopy, 0, static_cast<uint8_t>(mode)};
            for (int size = 4; size <= 18; ++size) t[i++][0] = {kCopy, static_cast<uint8_t>(size), static_cast<uint8_t>(mode)};
        }
        for (int mode = 0; mode <= 5; ++mode) {
            for (int add = 1; add <= 4; ++add) {
                for (int copy = 4; copy <= 6; ++copy) {
                    t[i][0] = {kAdd, static_cast<uint8_t>(add), 0};
                    t[i++][1] = {kCopy, static_cast<uint8_t>(copy), static_cast<uint8_t>(mode)};
                }
            }
        }
        for (int mode = 6; mode <= 8; ++mode) {
            for (int add = 1; add <= 4; ++add) {
                t[i][0] = {kAdd, static_cast<uint8_t>(add), 0};
                t[i++][1] = {kCopy, 4, static_cast<uint8_t>(mode)};
            }
        }
        for (int mode = 0; mode <= 8; ++mode) {
            t[i][0] = {kCopy, 4, static_cast<uint8_t>(mode)};
            t[i++][1] = {kAdd, 1, 0};
        }
        return t;
    }();
    return table;
}

struct Reader {
    const uint8_t* at;
    const uint8_t* end;
    bool ok = true;

    uint8_t byte() {
        if (at >= end) return ok = false, 0;
        return *at++;
    }
    uint64_t integer() {  // base 128, most significant first
        uint64_t value = 0;
        for (int i = 0; i < 10; ++i) {
            const uint8_t b = byte();
            value = (value << 7) | (b & 0x7F);
            if (!(b & 0x80)) return value;
        }
        return ok = false, 0;
    }
    Reader take(uint64_t size) {
        if (size > static_cast<uint64_t>(end - at)) return ok = false, Reader{end, end};
        Reader part{at, at + size};
        at += size;
        return part;
    }
};

}  // namespace

bool vcdiff_decode(const uint8_t* delta, size_t size, const VcdiffSource& source, const VcdiffSink& sink,
                   std::string* error) {
    const auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    Reader r{delta, delta + size};
    if (size < 5 || std::memcmp(delta, "\xD6\xC3\xC4\x00", 4) != 0) return fail("not a VCDIFF delta");
    r.at += 4;
    const uint8_t header = r.byte();
    if (header & 0x01) return fail("secondary compression is not supported");
    if (header & 0x02) return fail("a custom code table is not supported");
    if (header & 0x04) r.take(r.integer());  // xdelta3's application header
    if (header & ~0x07) return fail("unknown header bits");
    const CodeTable& table = default_table();
    std::vector<uint8_t> target, segment;
    while (r.ok && r.at < r.end) {
        const uint8_t window = r.byte();
        if (window & 0x02) return fail("copying from the target is not supported");
        uint64_t segment_size = 0, segment_offset = 0;
        if (window & 0x01) segment_size = r.integer(), segment_offset = r.integer();
        r.integer();  // the delta encoding's length
        const uint64_t target_size = r.integer();
        if (r.byte() != 0) return fail("compressed sections are not supported");
        const uint64_t data_size = r.integer(), inst_size = r.integer(), addr_size = r.integer();
        if (window & 0x04) r.take(4);  // xdelta3's adler32 of the target window
        Reader data = r.take(data_size), inst = r.take(inst_size), addr = r.take(addr_size);
        if (!r.ok || target_size > (64u << 20) || segment_size > (64u << 20)) return fail("damaged window");
        segment.resize(static_cast<size_t>(segment_size));
        if (segment_size && !source(segment_offset, segment.data(), segment.size())) return fail("cannot read the source");
        target.clear();
        target.reserve(static_cast<size_t>(target_size));
        uint64_t near[kNear] = {}, same[kSame * 256] = {};
        int next_near = 0;
        while (inst.ok && inst.at < inst.end) {
            const auto& pair = table[inst.byte()];
            for (const Instruction& in : pair) {
                if (in.type == kNoop) continue;
                const uint64_t n = in.size ? in.size : inst.integer();
                if (target.size() + n > target_size) return fail("window overflow");
                if (in.type == kAdd) {
                    Reader bytes = data.take(n);
                    if (!data.ok) return fail("damaged data section");
                    target.insert(target.end(), bytes.at, bytes.end);
                } else if (in.type == kRun) {
                    target.insert(target.end(), static_cast<size_t>(n), data.byte());
                } else {
                    const uint64_t here = segment_size + target.size();
                    uint64_t address;
                    if (in.mode == 0) {
                        address = addr.integer();
                    } else if (in.mode == 1) {
                        address = here - addr.integer();
                    } else if (in.mode < 2 + kNear) {
                        address = near[in.mode - 2] + addr.integer();
                    } else {
                        address = same[(in.mode - 2 - kNear) * 256 + addr.byte()];
                    }
                    near[next_near] = address;
                    next_near = (next_near + 1) % kNear;
                    same[address % (kSame * 256)] = address;
                    if (address >= here) return fail("copy from ahead");
                    if (address + n <= segment_size) {  // most of a translation: the disc's bytes as they are
                        target.insert(target.end(), segment.begin() + static_cast<ptrdiff_t>(address),
                                      segment.begin() + static_cast<ptrdiff_t>(address + n));
                        continue;
                    }
                    for (uint64_t k = 0; k < n; ++k, ++address) {  // a copy from the target may overlap what it writes
                        target.push_back(address < segment_size ? segment[static_cast<size_t>(address)]
                                                                : target[static_cast<size_t>(address - segment_size)]);
                    }
                }
            }
        }
        if (!data.ok || !inst.ok || !addr.ok || target.size() != target_size) return fail("damaged instructions");
        if (!sink(target.data(), target.size())) return fail("cannot write the target");
    }
    if (!r.ok) return fail("damaged delta");
    return true;
}

}  // namespace xport::pc
