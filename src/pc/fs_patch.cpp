#include "pc/fs_patch.h"

#include <map>
#include <optional>
#include <tuple>

#include "core/byteorder.h"
#include "pc/log.h"
#include "pc/xbox_thread.h"

namespace mc3::pc {
namespace {

enum class Load : uint8_t { Dword, ZeroExtendByte };

struct Site {
    uint32_t length;
    Load load;
    uint8_t reg;  // destination register number (0 = eax ... 7 = edi)
    uint32_t offset;
};

constexpr uint8_t kRegEsp = 4;
constexpr uint8_t kFsPrefix = 0x64;
constexpr uint8_t kNop = 0x90;

// mov eax, fs:[disp32] | mov r32, fs:[disp32] | movzx r32, byte fs:[disp32]
std::optional<Site> decode(const uint8_t* p, const uint8_t* end) {
    const ptrdiff_t available = end - p;
    if (available < 6 || p[0] != kFsPrefix) return std::nullopt;
    if (p[1] == 0xA1) return Site{6, Load::Dword, 0, load_le32(p + 2)};
    if (available >= 7 && p[1] == 0x8B && (p[2] & 0xC7) == 0x05) {
        return Site{7, Load::Dword, static_cast<uint8_t>((p[2] >> 3) & 7), load_le32(p + 3)};
    }
    if (available >= 8 && p[1] == 0x0F && p[2] == 0xB6 && (p[3] & 0xC7) == 0x05) {
        return Site{8, Load::ZeroExtendByte, static_cast<uint8_t>((p[3] >> 3) & 7), load_le32(p + 4)};
    }
    return std::nullopt;
}

// mov reg, fs:[slot] ; mov/movzx reg, [reg + offset] ; ret
// Only the destination register is written and mov does not touch flags, so the call is transparent.
uint8_t* build_helper(ExecMemory& memory, const Site& site) {
    uint8_t* code = memory.allocate(16);
    if (!code) return nullptr;
    uint8_t* p = code;
    *p++ = kFsPrefix;
    if (site.reg == 0) {
        *p++ = 0xA1;
    } else {
        *p++ = 0x8B;
        *p++ = static_cast<uint8_t>(0x05 | (site.reg << 3));
    }
    store_le32(p, kXboxContextFsSlot);
    p += 4;
    if (site.load == Load::ZeroExtendByte) {
        *p++ = 0x0F;
        *p++ = 0xB6;
    } else {
        *p++ = 0x8B;
    }
    *p++ = static_cast<uint8_t>(0x80 | (site.reg << 3) | site.reg);  // [reg + disp32]; esp is never a destination
    store_le32(p, site.offset);
    p += 4;
    *p++ = 0xC3;
    return code;
}

}  // namespace

FsPatchReport patch_fs_reads(const Xbe& xbe, ExecMemory& memory) {
    FsPatchReport report;
    std::map<std::tuple<Load, uint8_t, uint32_t>, uint8_t*> helpers;

    for (const XbeSection& section : xbe.sections()) {
        // TODO(phase 2): sections loaded later through XeLoadSection need the same pass.
        if (!section.executable() || !section.preload()) continue;
        auto* p = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(section.virtual_address));
        const uint8_t* end = p + (section.raw_size < section.virtual_size ? section.raw_size : section.virtual_size);

        while (p < end) {
            const std::optional<Site> site = decode(p, end);
            if (!site || site->offset == 0 || site->offset >= kXboxKpcrSize) {
                ++p;
                continue;
            }
            if (site->offset == kXboxContextFsSlot || site->reg == kRegEsp) {
                log_line("fs patch: skipped 0x%08X (fs:[0x%02X] into r%u)", address_of(p), site->offset, site->reg);
                ++report.skipped;
                p += site->length;
                continue;
            }

            uint8_t*& helper = helpers[std::make_tuple(site->load, site->reg, site->offset)];
            if (!helper) helper = build_helper(memory, *site);
            if (!helper) {
                log_line("fs patch: out of executable memory");
                return report;
            }
            emit_call(p, helper);
            for (uint32_t i = 5; i < site->length; ++i) p[i] = kNop;
            log_line("fs patch: 0x%08X fs:[0x%02X] -> r%u (%s) [%s]", address_of(p), site->offset, site->reg,
                     site->load == Load::ZeroExtendByte ? "movzx byte" : "mov dword", section.name.c_str());
            ++report.patched;
            p += site->length;
        }
    }
    report.helpers = static_cast<uint32_t>(helpers.size());
    return report;
}

}  // namespace mc3::pc
