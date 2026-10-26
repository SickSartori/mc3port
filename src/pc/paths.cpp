#include "pc/paths.h"

#include <windows.h>
#include <shlobj.h>

namespace mc3::pc {

std::filesystem::path program_directory() {
    static const std::filesystem::path directory = [] {
        wchar_t path[MAX_PATH * 4];
        const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        return std::filesystem::path(std::wstring(path, length)).parent_path();
    }();
    return directory;
}

bool portable_mode() {
    static const bool portable = [] {
        std::error_code ignored;
        const std::filesystem::path program = program_directory();
        return std::filesystem::exists(program / "portable.txt", ignored) ||
               std::filesystem::exists(program / "mc3port.ini", ignored);
    }();
    return portable;
}

std::filesystem::path data_directory() {
    static const std::filesystem::path directory = [] {
        if (portable_mode()) return program_directory();
        PWSTR local = nullptr;
        std::filesystem::path result = program_directory();
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
            std::error_code error;
            std::filesystem::create_directories(std::filesystem::path(local) / "mc3port", error);
            if (!error) result = std::filesystem::path(local) / "mc3port";
        }
        CoTaskMemFree(local);
        return result;
    }();
    return directory;
}

}  // namespace mc3::pc
