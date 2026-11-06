#include "pc/paths.h"

#include <windows.h>
#include <shlobj.h>

#include <string>
#include <system_error>
#include <vector>

namespace mc3::pc {
namespace {

std::string g_data_folder_note;
bool g_old_data_in_use = false, g_old_data_move_failed = false;

// A folder of ours, not one merely made by hand or by an older run.
bool has_data(const std::filesystem::path& folder) {
    std::error_code error;
    return std::filesystem::exists(folder / "xbox_hdd", error) || std::filesystem::exists(folder / "mc3port.ini", error);
}

// By renaming; across drives (Documents can be moved to another one) by a copy under another name, so that a copy
// that fails halfway never looks like the real thing, and the original goes only when the copy is complete.
bool move_item(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code error;
    std::filesystem::rename(from, to, error);
    if (!error) return true;
    std::filesystem::path partial = to;
    partial += L".copying";
    std::filesystem::remove_all(partial, error);
    std::filesystem::copy(from, partial, std::filesystem::copy_options::recursive, error);
    if (!error) std::filesystem::rename(partial, to, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove_all(partial, ignored);
        return false;
    }
    std::filesystem::remove_all(from, error);  // what stays is only left over: the game uses the copy
    return true;
}

std::filesystem::path known_folder(const KNOWNFOLDERID& id) {
    PWSTR folder = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &folder))) result = folder;
    CoTaskMemFree(folder);
    return result;
}

}  // namespace

const std::string& data_folder_note() { return g_data_folder_note; }

bool old_data_in_use() {
    data_directory();
    return g_old_data_in_use;
}

bool old_data_move_failed() {
    data_directory();
    return g_old_data_move_failed;
}

bool move_data_folder(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code error;
    if (!std::filesystem::is_directory(from, error)) return true;
    std::filesystem::create_directories(to, error);
    if (error) return false;
    std::vector<std::filesystem::path> items;
    if (std::filesystem::exists(from / "xbox_hdd", error)) items.push_back(from / "xbox_hdd");
    for (const auto& item : std::filesystem::directory_iterator(from, error)) {
        if (item.path().filename() != "xbox_hdd") items.push_back(item.path());
    }
    if (error) return false;
    for (const std::filesystem::path& item : items) {
        const std::filesystem::path target = to / item.filename();
        if (std::filesystem::exists(target, error)) continue;
        if (!move_item(item, target)) return false;
    }
    std::filesystem::remove(from, error);  // only if empty
    return true;
}

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
        std::error_code error;
        const std::filesystem::path documents = known_folder(FOLDERID_Documents), local = known_folder(FOLDERID_LocalAppData);
        const std::filesystem::path old_folder = local.empty() ? local : local / "mc3port";
        if (!documents.empty()) {
            const std::filesystem::path folder = documents / "mc3port";
            if (!old_folder.empty() && has_data(old_folder) && !has_data(folder)) {
                const bool asked = GetEnvironmentVariableW(L"MC3_MOVE_OLD_DATA", nullptr, 0) != 0;
                SetEnvironmentVariableW(L"MC3_MOVE_OLD_DATA", nullptr);  // not for a later restart
                if (asked && move_data_folder(old_folder, folder)) {
                    g_data_folder_note = "old data folder moved here: " + old_folder.string();
                    return folder;
                }
                g_old_data_move_failed = asked;
                if (asked) g_data_folder_note = "old data folder could not be moved: " + old_folder.string();
                // Part of it may have moved, the saves first: then the new folder is the one with the saves.
                if (has_data(folder)) return folder;
                g_old_data_in_use = true;
                return old_folder;
            }
            std::filesystem::create_directories(folder, error);
            if (!error) return folder;
        }
        if (!old_folder.empty()) {
            std::filesystem::create_directories(old_folder, error);
            if (!error) return old_folder;
        }
        return program_directory();
    }();
    return directory;
}

}  // namespace mc3::pc
