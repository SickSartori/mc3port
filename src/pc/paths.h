#pragma once

// Where mc3port reads and writes. The program may be installed in Program Files, where nothing can be written, so
// its own files (settings, saves, logs, shader cache) go to Documents\mc3port, where a player
// finds them. Up to 0.3.0 they were in %LOCALAPPDATA%\mc3port: they stay in use there until the player moves them from
// the launcher, so that nothing changes place unasked. A portable copy keeps them next
// to mc3.exe instead: one with a portable.txt there, or one that already has its mc3port.ini there (older versions).

#include <filesystem>
#include <string>

namespace mc3::pc {

std::filesystem::path program_directory();  // mc3.exe's folder
std::filesystem::path data_directory();     // created if missing
inline std::filesystem::path log_directory() { return data_directory() / "logs"; }  // the logs and crash reports
bool portable_mode();
// What this start did with the old folder, for the log; empty if nothing.
const std::string& data_folder_note();
// The data of an earlier version is in use, in the old folder: the launcher offers to move it. To do it, a new process
// starts with MC3_MOVE_OLD_DATA set, before anything of it has opened those files.
bool old_data_in_use();
bool old_data_move_failed();  // this start was asked to move it and could not
// Moves what is in `from` to `to`, the saves first, and never over a file that exists there. False if something could
// not be moved: it stays in `from`, and the rest is not tried. A folder left empty goes.
bool move_data_folder(const std::filesystem::path& from, const std::filesystem::path& to);

}  // namespace mc3::pc
