#pragma once

// The launcher: the game window's first screen, before the game starts. Disc check and first-run guide, settings,
// keyboard keys, version and updates; in the game's colours, with its logo and artwork read from the player's disc.

#include <windows.h>

#include <filesystem>
#include <optional>

#include "pc/game_disc.h"

namespace mc3::pc {

// Runs until the player starts the game (returns the verified disc) or quits (returns nothing).
std::optional<GameDisc> run_launcher(HWND window, const std::filesystem::path& exe_directory);

}  // namespace mc3::pc
