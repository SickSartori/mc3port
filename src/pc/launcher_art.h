#pragma once

// Pictures for the launcher, read from the player's own disc (never shipped with the port): the game's logo and
// the artwork of its loading screens. They live in the menus' Flash files (flash/*.xbck), as Xbox DXT5 textures.

#include <cstdint>
#include <vector>

#include "core/xdvdfs.h"

namespace mc3::pc {

struct DxtImage {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> blocks;  // DXT5 (BC3) blocks, row by row
};

struct LauncherArt {
    DxtImage logo;                  // 512x128, with alpha
    std::vector<DxtImage> artwork;  // 512x256 each, shuffled
};

// Reads a few megabytes from the disc: run it on a helper thread.
LauncherArt load_launcher_art(const XdvdfsImage& disc);

}  // namespace mc3::pc
