#pragma once

#include "core/xbe.h"

namespace mc3::pc {

// Redirects the DirectSound library's entry points to the software mixer (audio_mixer.cpp). Only for the XBE whose
// addresses were analysed (MC3 PAL, DSOUND 5849); any other XBE keeps its own DirectSound.
void install_dsound_hle(const Xbe& xbe);

}  // namespace mc3::pc
