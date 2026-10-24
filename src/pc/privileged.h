#pragma once

#include <windows.h>

namespace mc3::pc {

// The Xbox runs games in ring 0: the libraries use port I/O, cli/sti, wbinvd and MSRs directly. On Windows those
// raise STATUS_PRIVILEGED_INSTRUCTION; this emulates the instruction at context->Eip and steps over it.
// Returns false for instructions it does not know.
bool emulate_privileged_instruction(CONTEXT* context);

}  // namespace mc3::pc
