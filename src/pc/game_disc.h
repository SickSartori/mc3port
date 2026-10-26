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

// Opens `path` if it holds the supported game: the PAL Midnight Club 3: DUB Edition Remix, whose code the port's
// patches are made for (MC3_ANY_VERSION=1 accepts any). Otherwise `problem` says why, for the player.
std::optional<GameDisc> check_game_disc(const std::filesystem::path& path, std::string& problem);

// The disc image given on the command line (empty if none).
std::filesystem::path command_line_disc();

// Without asking: the command line's disc image, else the one remembered in mc3port.ini (disc_image), else an .iso
// next to mc3.exe or in the working directory. `problem` tells why the last one tried was refused.
std::optional<GameDisc> find_game_disc(const std::filesystem::path& exe_directory, std::string& problem);

// The file dialog for choosing a disc image (empty path if closed).
std::filesystem::path ask_for_disc_image(void* owner_window);

// find_game_disc, then message boxes and the file dialog until a good disc is chosen (then remembered). Returns
// nothing when the player gives up (the caller exits). Used when the launcher is skipped.
std::optional<GameDisc> open_game_disc(const std::filesystem::path& exe_directory);

}  // namespace mc3::pc
