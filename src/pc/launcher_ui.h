#pragma once

// The launcher: the game window's first screen, before the game starts. Disc check and first-run guide, settings,
// keyboard keys, version and updates; in the game's colours, with its logo and artwork read from the player's disc.
// The same pages come back during the game as the in-game menu.

#include <windows.h>

#include <filesystem>
#include <optional>

#include "pc/game_disc.h"

namespace mc3::pc {

// Runs until the player starts the game (returns the verified disc) or quits (returns nothing).
std::optional<GameDisc> run_launcher(HWND window, const std::filesystem::path& exe_directory);

// The in-game menu: the launcher's settings and controls over the paused game, opened with F1 or a controller's
// Back+Start, and when the window goes to the background (setting pause_in_background). Also draws the frame
// counter (setting fps_counter). Call once the window and the renderer exist; `disc_path` gives the logo.
void start_game_menu(HWND window, const std::filesystem::path& disc_path);
// Opens the menu (a controller's Back+Start).
void game_menu_open();

}  // namespace mc3::pc
