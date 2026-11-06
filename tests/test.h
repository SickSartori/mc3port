#pragma once

// A check that fails prints where and carries on, so one run shows every failure. main returns test_result().

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

inline int g_failures = 0;

#define CHECK(condition)                                                          \
    do {                                                                          \
        if (!(condition)) {                                                       \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);    \
            ++g_failures;                                                         \
        }                                                                         \
    } while (false)

inline int test_result() {
    std::printf(g_failures ? "%d checks failed\n" : "all checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}

// An empty folder of its own, removed with the object.
struct TemporaryFolder {
    std::filesystem::path path;
    explicit TemporaryFolder(const char* name) {
        path = std::filesystem::temp_directory_path() / name;
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TemporaryFolder() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

inline void write_file(const std::filesystem::path& path, const void* data, size_t size) {
    std::ofstream file(path, std::ios::binary);
    file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
}
inline void write_file(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
    write_file(path, data.data(), data.size());
}
inline void write_file(const std::filesystem::path& path, const std::string& data) {
    write_file(path, data.data(), data.size());
}
inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}
