#include "pc/log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace mc3::pc {
namespace {

std::mutex g_mutex;
std::FILE* g_file = nullptr;

}  // namespace

void log_open(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) g_file = _wfopen(path.c_str(), L"w");
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

}  // namespace mc3::pc
