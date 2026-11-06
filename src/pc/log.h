#pragma once

#include <filesystem>

namespace mc3::pc {

// Lines go to stderr and to the log file, flushed immediately: the process may die on the next instruction.
// The two runs before stay next to it: mc3port.log becomes mc3port.1.log, that one mc3port.2.log.
void log_open(const std::filesystem::path& path);
void rotate_log(const std::filesystem::path& path);
void log_line(const char* format, ...);

// Ends the process at once, every thread with it, like the Xbox's power button. ExitProcess would run the DLLs'
// detach code and static destructors first: under Wine while the game's threads still run on (one was left
// spinning on faults at 100%), and on Windows sometimes with a memory error in the log. Nothing is lost: the log is
// flushed line by line, settings and shaders are written when they change.
[[noreturn]] void end_process(unsigned code);

}  // namespace mc3::pc
