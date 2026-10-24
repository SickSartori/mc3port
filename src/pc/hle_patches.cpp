#include "pc/hle_patches.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "pc/log.h"

namespace mc3::pc {
namespace {

struct Patch {
    const char* name;
    uint32_t address;
    std::vector<uint8_t> original;  // checked before patching
    std::vector<uint8_t> replacement;
};

// XAPI input functions (XPP library, XDK 5849), found from the game's call sequence at 0x84B40:
//   XInitDevices(0, 0); while (XGetDeviceEnumerationStatus() == BUSY); XGetDevices(XDEVICE_TYPE_GAMEPAD); ...
// XInitDevices would start the USB host controller driver; instead nothing is enumerated for now, so the game
// sees no controllers. Host input (XInput) will plug in at XGetDevices/XInputOpen/XInputGetState.
const Patch kPatches[] = {
    {"XInitDevices", 0x0043F0E7, {0x55, 0x8D, 0x6C, 0x24, 0x90}, {0xC2, 0x08, 0x00}},       // ret 8
    {"XGetDeviceEnumerationStatus", 0x004411A2, {0x56, 0x33, 0xF6}, {0x33, 0xC0, 0xC3}},   // return IDLE (0)
};

}  // namespace

void apply_hle_patches(const Xbe& /*xbe*/) {
    for (const Patch& patch : kPatches) {
        auto* code = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(patch.address));
        if (std::memcmp(code, patch.original.data(), patch.original.size()) != 0) {
            log_line("HLE: %s at %08X does not match this XBE, not patched", patch.name, patch.address);
            continue;
        }
        std::memcpy(code, patch.replacement.data(), patch.replacement.size());
        log_line("HLE: %s at %08X replaced", patch.name, patch.address);
    }
}

}  // namespace mc3::pc
