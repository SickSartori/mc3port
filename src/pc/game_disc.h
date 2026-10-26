#pragma once

// The player's game disc: which disc image to run and whether it is the version the port is made for.

#include <filesystem>
#include <memory>
#include <optional>

#include "core/xbe.h"
#include "core/xdvdfs.h"

namespace mc3::pc {

struct GameDisc {
    std::filesystem::path path;
    std::unique_ptr<XdvdfsImage> image;
    Xbe xbe;
};

// The disc image from the command line, else the one remembered in mc3port.ini (disc_image), else an .iso next
// to mc3.exe or in the working directory, else one the player picks in a file dialog (then remembered). Each must
// hold the supported game: the PAL Midnight Club 3: DUB Edition Remix, whose code the port's patches are made
// for (MC3_ANY_VERSION=1 accepts any). Returns nothing when the player gives up (the caller exits).
std::optional<GameDisc> open_game_disc(const std::filesystem::path& exe_directory);

}  // namespace mc3::pc
