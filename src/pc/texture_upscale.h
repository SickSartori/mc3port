#pragma once

// Setting sharp_2d: the textures of flat draws over the 3D scene (the HUD, the radar's map, icons, letters) made two
// or four times larger with their edges kept, so that they stay sharp at a high resolution. The game's pictures are
// small and were shown at 480 lines; enlarged by the graphics card they turn soft and show the 4x4 blocks of their
// compression. Nothing is added that is not there: edges come out clean, details stay as they were.

#include <vector>

#include "pc/nv2a_texture.h"

namespace mc3::pc {

// The enlarged texture's levels (BGRA8, the top one first, down to 1x1), or none when it is not worth it or the
// layout is unknown.
std::vector<TextureImage> upscale_2d_texture(const TextureImage& image);

}  // namespace mc3::pc
