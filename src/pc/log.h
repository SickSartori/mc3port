#pragma once

#include <filesystem>

namespace mc3::pc {

// Lines go to stderr and to the log file, flushed immediately: the process may die on the next instruction.
void log_open(const std::filesystem::path& path);
void log_line(const char* format, ...);

}  // namespace mc3::pc
