#pragma once

// Where mc3port reads and writes. The program may be installed in Program Files, where nothing can be written, so
// its own files (settings, saves, log, shader cache) go to %LOCALAPPDATA%\mc3port. A portable copy keeps them next
// to mc3.exe instead: one with a portable.txt there, or one that already has its mc3port.ini there (older versions).

#include <filesystem>

namespace mc3::pc {

std::filesystem::path program_directory();  // mc3.exe's folder
std::filesystem::path data_directory();     // created if missing
bool portable_mode();

}  // namespace mc3::pc
