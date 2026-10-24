#pragma once

#include <cstdint>

#include "core/xbe.h"
#include "pc/exec_memory.h"

namespace mc3::pc {

struct FsPatchReport {
    uint32_t patched = 0;
    uint32_t skipped = 0;
    uint32_t helpers = 0;
};

// Rewrites the game's reads of fs:[offset] (offset != 0) in the mapped image into calls to generated helpers
// that read the emulated KPCR. fs:[0], the SEH chain, means the same on Windows and stays native.
//
// Matching is a byte-pattern scan, not a disassembly: every patched address is logged so it can be checked.
FsPatchReport patch_fs_reads(const Xbe& xbe, ExecMemory& memory);

}  // namespace mc3::pc
