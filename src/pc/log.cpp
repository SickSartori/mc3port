#include "pc/log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <system_error>

namespace mc3::pc {
namespace {

std::mutex g_mutex;
std::FILE* g_file = nullptr;

}  // namespace

void rotate_log(const std::filesystem::path& path) {
    const auto older = [&](int age) {
        std::filesystem::path name = path;
        return name.replace_extension(std::to_wstring(age) + path.extension().wstring());
    };
    std::error_code ignored;  // a log that is missing or in use stays where it is
    std::filesystem::rename(older(1), older(2), ignored);
    std::filesystem::rename(path, older(1), ignored);
}

void log_open(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) return;
    rotate_log(path);
    g_file = _wfopen(path.c_str(), L"w");
}

void log_line(const char* format, ...) {
    char line[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(g_mutex);
    std::fprintf(stderr, "%s\n", line);
    if (g_file) {
        std::fprintf(g_file, "%s\n", line);
        std::fflush(g_file);
    }
}

void end_process(unsigned code) {
    TerminateProcess(GetCurrentProcess(), code);
    for (;;) Sleep(INFINITE);  // TerminateProcess does not return for the current process
}

}  // namespace mc3::pc
