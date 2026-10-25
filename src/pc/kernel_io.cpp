// Xbox file system on the host.
//
//   \Device\CdRom0                  -> the disc image, read-only, served straight from the .iso
//   \Device\Harddisk0\PartitionN    -> <host data directory>\PartitionN (saves, title data, caches)
//
// Drive letters (D:, T:, U:, Z: ...) are symbolic links to those devices, created by the kernel (D:) or by
// XAPI at startup through IoCreateSymbolicLink.

#include <windows.h>
#include <malloc.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "pc/handles.h"
#include "pc/kernel_internal.h"
#include "pc/log.h"
#include "pc/physical_memory.h"

namespace mc3::pc {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kFileOpen = 1;
constexpr uint32_t kFileCreate = 2;
constexpr uint32_t kFileOpenIf = 3;
constexpr uint32_t kFileOverwrite = 4;
constexpr uint32_t kFileOverwriteIf = 5;
constexpr uint32_t kFileSupersede = 0;

constexpr uint32_t kFileDirectoryFile = 0x1;
constexpr uint32_t kFileNonDirectoryFile = 0x40;
constexpr uint32_t kFileDeleteOnClose = 0x1000;

constexpr uint32_t kInfoSuperseded = 0;
constexpr uint32_t kInfoOpened = 1;
constexpr uint32_t kInfoCreated = 2;
constexpr uint32_t kInfoOverwritten = 3;

constexpr uint32_t kWriteAccessMask = 0x2 | 0x4 | 0x100 | 0x10000 | 0x10000000 | 0x40000000;

constexpr uint32_t kAttributeReadOnly = 0x01;
constexpr uint32_t kAttributeDirectory = 0x10;
constexpr uint32_t kAttributeNormal = 0x80;

constexpr int64_t kUseFilePointerPosition = -2;  // ByteOffset {0xFFFFFFFE, -1}
constexpr int64_t kWriteToEndOfFile = -1;        // ByteOffset {0xFFFFFFFF, -1}

constexpr uint32_t kClusterSize = 16 * 1024;
constexpr uint32_t kBytesPerSector = 512;

enum class Device { None, Disc, Partition };

struct Location {
    Device device = Device::None;
    uint32_t partition = 0;
    std::vector<std::string> components;  // below the device root
    std::string xbox_path;                // canonical: device path + components
};

struct FileInfo {
    int64_t creation_time = 0;
    int64_t last_access_time = 0;
    int64_t last_write_time = 0;
    int64_t change_time = 0;
    uint64_t end_of_file = 0;
    uint64_t allocation_size = 0;
    uint32_t attributes = kAttributeNormal;
    bool directory = false;
};

struct DirectoryItem {
    std::string name;
    FileInfo info;
};

}  // namespace

struct FileObject {
    enum class Kind { DiscFile, DiscDirectory, HostFile, HostDirectory, Volume } kind = Kind::Volume;
    Location location;
    XdvdfsImage::Entry disc_entry;
    HANDLE host = INVALID_HANDLE_VALUE;
    fs::path host_path;
    uint64_t position = 0;
    bool delete_on_close = false;
    std::mutex mutex;

    std::vector<DirectoryItem> listing;
    size_t next_item = 0;
    bool listed = false;

    ~FileObject() {
        if (host != INVALID_HANDLE_VALUE) CloseHandle(host);
        if (delete_on_close) {
            std::error_code ignored;
            fs::remove(host_path, ignored);
        }
    }
};

namespace {

const XdvdfsImage* g_disc = nullptr;
fs::path g_host_root;

std::mutex g_links_mutex;
std::unordered_map<std::string, std::string> g_symbolic_links;  // lower-case "\??\x:" -> device path

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool starts_with_ignore_case(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && lower(text.substr(0, prefix.size())) == lower(prefix);
}

std::string ansi_string(const XAnsiString* string) {
    if (!string || !string->buffer) return {};
    return std::string(guest<const char>(string->buffer), string->length);
}

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> components;
    std::string current;
    for (char c : path) {
        if (c == '\\' || c == '/') {
            if (!current.empty()) components.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) components.push_back(std::move(current));
    std::vector<std::string> normalized;
    for (std::string& component : components) {
        if (component == ".") continue;
        if (component == "..") {
            if (!normalized.empty()) normalized.pop_back();
            continue;
        }
        normalized.push_back(std::move(component));
    }
    return normalized;
}

std::string join_xbox(const std::string& device, const std::vector<std::string>& components) {
    std::string path = device;
    for (const std::string& component : components) path += "\\" + component;
    return path;
}

std::optional<Location> resolve_path(std::string path) {
    std::replace(path.begin(), path.end(), '/', '\\');

    // Drive letters: "\??\D:\x", also accepted without the "\??\" prefix.
    std::string link;
    if (starts_with_ignore_case(path, "\\??\\")) {
        const size_t end = path.find('\\', 4);
        link = path.substr(0, end);
        path = end == std::string::npos ? std::string{} : path.substr(end);
    } else if (path.size() >= 2 && path[1] == ':') {
        link = "\\??\\" + path.substr(0, 2);
        path = path.substr(2);
    }
    if (!link.empty()) {
        std::lock_guard<std::mutex> lock(g_links_mutex);
        const auto it = g_symbolic_links.find(lower(link));
        if (it == g_symbolic_links.end()) return std::nullopt;
        path = it->second + path;
    }

    Location location;
    const std::string kDisc = "\\Device\\CdRom0";
    const std::string kPartition = "\\Device\\Harddisk0\\Partition";
    std::string device;
    std::string rest;
    if (starts_with_ignore_case(path, kDisc)) {
        location.device = Device::Disc;
        device = kDisc;
        rest = path.substr(kDisc.size());
    } else if (starts_with_ignore_case(path, kPartition)) {
        size_t end = kPartition.size();
        while (end < path.size() && std::isdigit(static_cast<unsigned char>(path[end]))) ++end;
        if (end == kPartition.size()) return std::nullopt;
        location.device = Device::Partition;
        location.partition = static_cast<uint32_t>(std::stoul(path.substr(kPartition.size(), end - kPartition.size())));
        device = path.substr(0, end);
        rest = path.substr(end);
    } else {
        return std::nullopt;
    }
    if (!rest.empty() && rest[0] != '\\') return std::nullopt;
    location.components = split_path(rest);
    location.xbox_path = join_xbox(device, location.components);
    return location;
}

std::optional<Location> resolve_attributes(const XObjectAttributes* attributes) {
    constexpr uint32_t kDosDevicesDirectory = 0xFFFFFFFD;  // ObDosDevicesDirectory(): the "\??\" namespace
    std::string name = ansi_string(guest<XAnsiString>(attributes->object_name));
    if (attributes->root_directory == kDosDevicesDirectory) {
        // A name without a drive ("resources\vehicle\...", used for the car models) is relative to the title's
        // directory, D:.
        const bool has_drive = name.size() >= 2 && name[1] == ':';
        name = (has_drive || (!name.empty() && name[0] == '\\')) ? "\\??\\" + name : "\\??\\D:\\" + name;
    } else if (attributes->root_directory) {
        const std::optional<HandleEntry> root = lookup_handle(attributes->root_directory);
        if (!root || !root->file) return std::nullopt;
        name = root->file->location.xbox_path + "\\" + name;
    }
    return resolve_path(name);
}

fs::path host_path_of(const Location& location) {
    fs::path path = g_host_root / ("Partition" + std::to_string(location.partition));
    for (const std::string& component : location.components) {
        path /= fs::path(std::u8string(component.begin(), component.end()));
    }
    return path;
}

std::optional<XdvdfsImage::Entry> disc_entry_of(const Location& location) {
    std::string path;
    for (const std::string& component : location.components) path += "\\" + component;
    return g_disc->find(path);
}

int64_t file_time_of(const FILETIME& time) {
    return (static_cast<int64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

FileInfo disc_info(const XdvdfsImage::Entry& entry) {
    FileInfo info;
    info.directory = entry.is_directory;
    info.attributes = kAttributeReadOnly | (entry.is_directory ? kAttributeDirectory : 0);
    info.end_of_file = entry.is_directory ? 0 : entry.size;
    info.allocation_size = (info.end_of_file + 2047) & ~uint64_t{2047};
    return info;
}

std::optional<FileInfo> host_info(const fs::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return std::nullopt;
    FileInfo info;
    info.creation_time = file_time_of(data.ftCreationTime);
    info.last_access_time = file_time_of(data.ftLastAccessTime);
    info.last_write_time = file_time_of(data.ftLastWriteTime);
    info.change_time = info.last_write_time;
    info.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    info.attributes = info.directory ? kAttributeDirectory : kAttributeNormal;
    info.end_of_file = info.directory ? 0 : (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    info.allocation_size = (info.end_of_file + kClusterSize - 1) & ~uint64_t{kClusterSize - 1};
    return info;
}

std::optional<FileInfo> info_of(FileObject& file) {
    switch (file.kind) {
        case FileObject::Kind::DiscFile:
        case FileObject::Kind::DiscDirectory:
            return disc_info(file.disc_entry);
        case FileObject::Kind::HostFile:
        case FileObject::Kind::HostDirectory:
            return host_info(file.host_path);
        case FileObject::Kind::Volume: {
            FileInfo info;
            info.directory = true;
            info.attributes = kAttributeDirectory;
            return info;
        }
    }
    return std::nullopt;
}

std::optional<FileInfo> info_of(const Location& location) {
    if (location.device == Device::Disc) {
        const std::optional<XdvdfsImage::Entry> entry = disc_entry_of(location);
        if (!entry) return std::nullopt;
        return disc_info(*entry);
    }
    return host_info(host_path_of(location));
}

NTSTATUS status_from_win32(DWORD error) {
    switch (error) {
        case ERROR_FILE_NOT_FOUND: return kStatusObjectNameNotFound;
        case ERROR_PATH_NOT_FOUND: return kStatusObjectPathNotFound;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS: return kStatusObjectNameCollision;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION: return kStatusAccessDenied;
        case ERROR_DISK_FULL: return kStatusDiskFull;
        case ERROR_DIR_NOT_EMPTY: return kStatusDirectoryNotEmpty;
        default: return kStatusUnsuccessful;
    }
}

std::shared_ptr<FileObject> file_from_handle(uint32_t handle) {
    const std::optional<HandleEntry> entry = lookup_handle(handle);
    return entry ? entry->file : nullptr;
}

void complete(XIoStatusBlock* io_status, NTSTATUS status, uint32_t information) {
    if (io_status) {
        io_status->status = status;
        io_status->information = information;
    }
}

// Asynchronous requests complete synchronously: signal the event and run the APC right away.
void finish_request(uint32_t event, uint32_t apc_routine, uint32_t apc_context, XIoStatusBlock* io_status) {
    if (event) {
        if (const uint32_t object = object_from_handle(event)) set_event(object);
    }
    if (apc_routine) {
        reinterpret_cast<void(__stdcall*)(uint32_t, XIoStatusBlock*, uint32_t)>(static_cast<uintptr_t>(apc_routine))(
            apc_context, io_status, 0);
    }
}

NTSTATUS open_disc(const Location& location, uint32_t disposition, uint32_t options, FileObject& file,
                   uint32_t* information) {
    if (location.components.empty() && !(options & kFileDirectoryFile)) {
        file.kind = FileObject::Kind::Volume;
        *information = kInfoOpened;
        return kStatusSuccess;
    }
    const std::optional<XdvdfsImage::Entry> entry = disc_entry_of(location);
    if (!entry) {
        if (disposition == kFileCreate || disposition == kFileOpenIf || disposition == kFileOverwriteIf ||
            disposition == kFileSupersede) {
            return kStatusAccessDenied;  // read-only media
        }
        Location parent = location;
        parent.components.pop_back();
        return parent.components.empty() || disc_entry_of(parent) ? kStatusObjectNameNotFound
                                                                  : kStatusObjectPathNotFound;
    }
    if ((options & kFileDirectoryFile) && !entry->is_directory) return kStatusNotADirectory;
    if ((options & kFileNonDirectoryFile) && entry->is_directory) return kStatusFileIsADirectory;
    if (disposition == kFileCreate) return kStatusObjectNameCollision;
    if (disposition == kFileOverwrite || disposition == kFileOverwriteIf || disposition == kFileSupersede) {
        return kStatusAccessDenied;
    }
    file.kind = entry->is_directory ? FileObject::Kind::DiscDirectory : FileObject::Kind::DiscFile;
    file.disc_entry = *entry;
    *information = kInfoOpened;
    return kStatusSuccess;
}

NTSTATUS open_host(const Location& location, uint32_t access, uint32_t disposition, uint32_t options, FileObject& file,
                   uint32_t* information) {
    file.host_path = host_path_of(location);
    if (location.components.empty() && !(options & kFileDirectoryFile)) {
        // Raw sector access to a partition (Partition0 is the whole disk: XAPI keeps its cache partition
        // database there). Backed by a sparse file; never-written sectors read as zeros.
        const fs::path raw = g_host_root / ("Partition" + std::to_string(location.partition) + ".raw");
        file.host = CreateFileW(raw.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file.host == INVALID_HANDLE_VALUE) return status_from_win32(GetLastError());
        file.kind = FileObject::Kind::Volume;
        *information = kInfoOpened;
        return kStatusSuccess;
    }
    std::error_code error;
    const bool exists = fs::exists(file.host_path, error);
    const bool is_directory = exists && fs::is_directory(file.host_path, error);

    if (is_directory || (!exists && (options & kFileDirectoryFile))) {
        if ((options & kFileNonDirectoryFile) && is_directory) return kStatusFileIsADirectory;
        if (exists) {
            if (disposition == kFileCreate) return kStatusObjectNameCollision;
            *information = kInfoOpened;
        } else {
            if (disposition != kFileCreate && disposition != kFileOpenIf) return kStatusObjectNameNotFound;
            if (!fs::exists(file.host_path.parent_path(), error)) return kStatusObjectPathNotFound;
            if (!CreateDirectoryW(file.host_path.c_str(), nullptr)) return status_from_win32(GetLastError());
            *information = kInfoCreated;
        }
        file.kind = FileObject::Kind::HostDirectory;
        return kStatusSuccess;
    }
    if (exists && (options & kFileDirectoryFile)) return kStatusNotADirectory;

    DWORD creation = OPEN_EXISTING;
    switch (disposition) {
        case kFileSupersede: creation = CREATE_ALWAYS; break;
        case kFileOpen: creation = OPEN_EXISTING; break;
        case kFileCreate: creation = CREATE_NEW; break;
        case kFileOpenIf: creation = OPEN_ALWAYS; break;
        case kFileOverwrite: creation = TRUNCATE_EXISTING; break;
        case kFileOverwriteIf: creation = CREATE_ALWAYS; break;
        default: return kStatusInvalidParameter;
    }
    const bool writable = (access & kWriteAccessMask) != 0 || creation != OPEN_EXISTING;
    file.host = CreateFileW(file.host_path.c_str(), GENERIC_READ | (writable ? GENERIC_WRITE : 0) | DELETE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, creation,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file.host == INVALID_HANDLE_VALUE) return status_from_win32(GetLastError());
    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    switch (creation) {
        case CREATE_ALWAYS: *information = existed ? (disposition == kFileSupersede ? kInfoSuperseded : kInfoOverwritten)
                                                   : kInfoCreated; break;
        case OPEN_ALWAYS: *information = existed ? kInfoOpened : kInfoCreated; break;
        case CREATE_NEW: *information = kInfoCreated; break;
        case TRUNCATE_EXISTING: *information = kInfoOverwritten; break;
        default: *information = kInfoOpened; break;
    }
    file.kind = FileObject::Kind::HostFile;
    file.delete_on_close = (options & kFileDeleteOnClose) != 0;
    return kStatusSuccess;
}

NTSTATUS __stdcall NtCreateFile(uint32_t* file_handle, uint32_t desired_access, const XObjectAttributes* attributes,
                                XIoStatusBlock* io_status, const int64_t* /*allocation_size*/,
                                uint32_t /*file_attributes*/, uint32_t /*share_access*/, uint32_t disposition,
                                uint32_t options) {
    const std::string name = ansi_string(guest<XAnsiString>(attributes->object_name));
    const std::optional<Location> location = resolve_attributes(attributes);
    if (!location) {
        log_line("NtCreateFile: cannot resolve '%s' (root directory %08X)", name.c_str(), attributes->root_directory);
        complete(io_status, kStatusObjectPathNotFound, 0);
        return kStatusObjectPathNotFound;
    }
    auto file = std::make_shared<FileObject>();
    file->location = *location;
    uint32_t information = 0;
    const NTSTATUS status = location->device == Device::Disc
                                ? open_disc(*location, disposition, options, *file, &information)
                                : open_host(*location, desired_access, disposition, options, *file, &information);
    log_line("NtCreateFile('%s' -> %s, disposition %u, options %X) = %08X", name.c_str(),
             location->xbox_path.c_str(), disposition, options, static_cast<uint32_t>(status));
    complete(io_status, status, information);
    if (status < 0) return status;
    *file_handle = create_handle(HandleEntry{0, std::move(file)});
    return status;
}

NTSTATUS __stdcall NtOpenFile(uint32_t* file_handle, uint32_t desired_access, const XObjectAttributes* attributes,
                              XIoStatusBlock* io_status, uint32_t share_access, uint32_t open_options) {
    return NtCreateFile(file_handle, desired_access, attributes, io_status, nullptr, 0, share_access, kFileOpen,
                        open_options);
}

NTSTATUS __stdcall NtReadFile(uint32_t handle, uint32_t event, uint32_t apc_routine, uint32_t apc_context,
                              XIoStatusBlock* io_status, void* buffer, uint32_t length, const int64_t* byte_offset) {
    const std::shared_ptr<FileObject> file = file_from_handle(handle);
    if (!file) return kStatusInvalidHandle;
    std::lock_guard<std::mutex> lock(file->mutex);
    uint64_t offset = file->position;
    if (byte_offset && *byte_offset != kUseFilePointerPosition) offset = static_cast<uint64_t>(*byte_offset);

    NTSTATUS status = kStatusSuccess;
    uint32_t read = 0;
    if (file->kind == FileObject::Kind::DiscFile) {
        if (offset >= file->disc_entry.size && length) {
            status = kStatusEndOfFile;
        } else {
            read = static_cast<uint32_t>(g_disc->read(file->disc_entry, offset, buffer, length));
            if (read == 0 && length) status = kStatusUnsuccessful;
        }
    } else if (file->host != INVALID_HANDLE_VALUE) {
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(offset);
        overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD count = 0;
        const bool ok = ReadFile(file->host, buffer, length, &count, &overlapped) != 0;
        if (file->kind == FileObject::Kind::Volume) {
            std::memset(static_cast<uint8_t*>(buffer) + count, 0, length - count);
            count = length;
            log_line("raw read %s offset %llX length %X", file->location.xbox_path.c_str(),
                     static_cast<unsigned long long>(offset), length);
        } else if (!ok) {
            status = GetLastError() == ERROR_HANDLE_EOF ? kStatusEndOfFile : status_from_win32(GetLastError());
        } else if (count == 0 && length) {
            status = kStatusEndOfFile;
        }
        read = count;
    } else {
        log_line("NtReadFile on %s: not readable", file->location.xbox_path.c_str());
        status = kStatusInvalidParameter;
    }
    file->position = offset + read;
    // Windows' write watch does not see what its own I/O writes; textures loaded from disc must be re-read.
    mark_physical_written(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(buffer)), read);
    complete(io_status, status, read);
    finish_request(event, apc_routine, apc_context, io_status);
    return status;
}

NTSTATUS __stdcall NtWriteFile(uint32_t handle, uint32_t event, uint32_t apc_routine, uint32_t apc_context,
                               XIoStatusBlock* io_status, const void* buffer, uint32_t length,
                               const int64_t* byte_offset) {
    const std::shared_ptr<FileObject> file = file_from_handle(handle);
    if (!file) return kStatusInvalidHandle;
    if (file->host == INVALID_HANDLE_VALUE) {
        log_line("NtWriteFile on %s: read-only", file->location.xbox_path.c_str());
        complete(io_status, kStatusAccessDenied, 0);
        return kStatusAccessDenied;
    }
    std::lock_guard<std::mutex> lock(file->mutex);
    uint64_t offset = file->position;
    if (byte_offset && *byte_offset == kWriteToEndOfFile) {
        LARGE_INTEGER size;
        GetFileSizeEx(file->host, &size);
        offset = static_cast<uint64_t>(size.QuadPart);
    } else if (byte_offset && *byte_offset != kUseFilePointerPosition) {
        offset = static_cast<uint64_t>(*byte_offset);
    }
    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(offset);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD written = 0;
    const NTSTATUS status =
        WriteFile(file->host, buffer, length, &written, &overlapped) ? kStatusSuccess : status_from_win32(GetLastError());
    file->position = offset + written;
    if (file->kind == FileObject::Kind::Volume) {
        log_line("raw write %s offset %llX length %X", file->location.xbox_path.c_str(),
                 static_cast<unsigned long long>(offset), length);
    }
    complete(io_status, status, written);
    finish_request(event, apc_routine, apc_context, io_status);
    return status;
}

#pragma pack(push, 1)
struct XFileBasicInformation {
    int64_t creation_time, last_access_time, last_write_time, change_time;
    uint32_t file_attributes;
    uint32_t padding;
};
struct XFileStandardInformation {
    uint64_t allocation_size, end_of_file;
    uint32_t number_of_links;
    uint8_t delete_pending, directory;
    uint8_t padding[2];
};
struct XFileNetworkOpenInformation {
    int64_t creation_time, last_access_time, last_write_time, change_time;
    uint64_t allocation_size, end_of_file;
    uint32_t file_attributes;
    uint32_t padding;
};
struct XFileDirectoryInformation {
    uint32_t next_entry_offset, file_index;
    int64_t creation_time, last_access_time, last_write_time, change_time;
    uint64_t end_of_file, allocation_size;
    uint32_t file_attributes, file_name_length;
    char file_name[1];
};
struct XFileFsSizeInformation {
    uint64_t total_allocation_units, available_allocation_units;
    uint32_t sectors_per_allocation_unit, bytes_per_sector;
};
struct XFileFsFullSizeInformation {
    uint64_t total_allocation_units, caller_available_allocation_units, actual_available_allocation_units;
    uint32_t sectors_per_allocation_unit, bytes_per_sector;
};
struct XFileFsDeviceInformation {
    uint32_t device_type, characteristics;
};
#pragma pack(pop)

void fill_network_open(XFileNetworkOpenInformation* out, const FileInfo& info) {
    *out = XFileNetworkOpenInformation{info.creation_time, info.last_access_time, info.last_write_time,
                                       info.change_time,   info.allocation_size,  info.end_of_file,
                                       info.attributes,    0};
}

enum : uint32_t {
    kFileDirectoryInformationClass = 1,
    kFileBasicInformationClass = 4,
    kFileStandardInformationClass = 5,
    kFileDispositionInformationClass = 13,
    kFilePositionInformationClass = 14,
    kFileAllocationInformationClass = 19,
    kFileEndOfFileInformationClass = 20,
    kFileNetworkOpenInformationClass = 34,
};

NTSTATUS __stdcall NtQueryInformationFile(uint32_t handle, XIoStatusBlock* io_status, void* information,
                                          uint32_t length, uint32_t information_class) {
    const std::shared_ptr<FileObject> file = file_from_handle(handle);
    if (!file) return kStatusInvalidHandle;
    std::lock_guard<std::mutex> lock(file->mutex);
    const std::optional<FileInfo> info = info_of(*file);
    if (!info) {
        complete(io_status, kStatusUnsuccessful, 0);
        return kStatusUnsuccessful;
    }
    uint32_t size = 0;
    switch (information_class) {
        case kFileBasicInformationClass:
            size = sizeof(XFileBasicInformation);
            if (length < size) break;
            *static_cast<XFileBasicInformation*>(information) = XFileBasicInformation{
                info->creation_time, info->last_access_time, info->last_write_time, info->change_time,
                info->attributes, 0};
            break;
        case kFileStandardInformationClass:
            size = sizeof(XFileStandardInformation);
            if (length < size) break;
            *static_cast<XFileStandardInformation*>(information) = XFileStandardInformation{
                info->allocation_size, info->end_of_file, 1, file->delete_on_close ? uint8_t{1} : uint8_t{0},
                info->directory ? uint8_t{1} : uint8_t{0}, {}};
            break;
        case kFilePositionInformationClass:
            size = sizeof(uint64_t);
            if (length < size) break;
            std::memcpy(information, &file->position, size);
            break;
        case kFileNetworkOpenInformationClass:
            size = sizeof(XFileNetworkOpenInformation);
            if (length < size) break;
            fill_network_open(static_cast<XFileNetworkOpenInformation*>(information), *info);
            break;
        default:
            log_line("NtQueryInformationFile: class %u not implemented", information_class);
            complete(io_status, kStatusInvalidParameter, 0);
            return kStatusInvalidParameter;
    }
    if (length < size) {
        complete(io_status, kStatusBufferTooSmall, 0);
        return kStatusBufferTooSmall;
    }
    complete(io_status, kStatusSuccess, size);
    return kStatusSuccess;
}

NTSTATUS __stdcall NtSetInformationFile(uint32_t handle, XIoStatusBlock* io_status, const void* information,
                                        uint32_t /*length*/, uint32_t information_class) {
    const std::shared_ptr<FileObject> file = file_from_handle(handle);
    if (!file) return kStatusInvalidHandle;
    std::lock_guard<std::mutex> lock(file->mutex);
    NTSTATUS status = kStatusSuccess;
    switch (information_class) {
        case kFilePositionInformationClass:
            std::memcpy(&file->position, information, sizeof(uint64_t));
            break;
        case kFileEndOfFileInformationClass: {
            if (file->kind != FileObject::Kind::HostFile) {
                status = kStatusAccessDenied;
                break;
            }
            LARGE_INTEGER end;
            std::memcpy(&end.QuadPart, information, sizeof(end.QuadPart));
            if (!SetFilePointerEx(file->host, end, nullptr, FILE_BEGIN) || !SetEndOfFile(file->host)) {
                status = status_from_win32(GetLastError());
            }
            break;
        }
        case kFileDispositionInformationClass:
            if (file->kind == FileObject::Kind::DiscFile || file->kind == FileObject::Kind::DiscDirectory) {
                status = kStatusCannotDelete;
            } else {
                file->delete_on_close = *static_cast<const uint8_t*>(information) != 0;
            }
            break;
        case kFileBasicInformationClass:
        case kFileAllocationInformationClass:
            break;  // timestamps and preallocation do not matter on the host
        default:
            log_line("NtSetInformationFile: class %u not implemented", information_class);
            status = kStatusInvalidParameter;
            break;
    }
    complete(io_status, status, 0);
    return status;
}

NTSTATUS __stdcall NtQueryVolumeInformationFile(uint32_t handle, XIoStatusBlock* io_status, void* information,
                                                uint32_t length, uint32_t information_class) {
    const std::shared_ptr<FileObject> file = file_from_handle(handle);
    if (!file) return kStatusInvalidHandle;
    const bool disc = file->location.device == Device::Disc;
    // Plenty of room on the "hard disk", reported in 16 KB clusters like FATX.
    const uint64_t total_units = disc ? 0x4000000000ull / kClusterSize / 16 : 0x100000000ull / kClusterSize;
    const uint64_t free_units = disc ? 0 : 0x40000000ull / kClusterSize;
    uint32_t size = 0;
    switch (information_class) {
        case 3:  // FileFsSizeInformation
            size = sizeof(XFileFsSizeInformation);
            if (length >= size) {
                *static_cast<XFileFsSizeInformation*>(information) =
                    XFileFsSizeInformation{total_units, free_units, kClusterSize / kBytesPerSector, kBytesPerSector};
            }
            break;
        case 7:  // FileFsFullSizeInformation
            size = sizeof(XFileFsFullSizeInformation);
            if (length >= size) {
                *static_cast<XFileFsFullSizeInformation*>(information) = XFileFsFullSizeInformation{
                    total_units, free_units, free_units, kClusterSize / kBytesPerSector, kBytesPerSector};
            }
            break;
        case 4:  // FileFsDeviceInformation
            size = sizeof(XFileFsDeviceInformation);
            if (length >= size) {
                constexpr uint32_t kDeviceTypeCdRom = 0x02;
                constexpr uint32_t kDeviceTypeDisk = 0x07;
                *static_cast<XFileFsDeviceInformation*>(information) =
                    XFileFsDeviceInformation{disc ? kDeviceTypeCdRom : kDeviceTypeDisk, 0};
            }
            break;
        default:
            log_line("NtQueryVolumeInformationFile: class %u not implemented", information_class);
            complete(io_status, kStatusInvalidParameter, 0);
            return kStatusInvalidParameter;
    }
    if (length < size) {
        complete(io_status, kStatusBufferTooSmall, 0);
        return kStatusBufferTooSmall;
    }
    complete(io_status, kStatusSuccess, size);
    return kStatusSuccess;
}

NTSTATUS __stdcall NtQueryFullAttributesFile(const XObjectAttributes* attributes,
                                             XFileNetworkOpenInformation* information) {
    const std::optional<Location> location = resolve_attributes(attributes);
    if (!location) return kStatusObjectPathNotFound;
    const std::optional<FileInfo> info = info_of(*location);
    if (!info) return kStatusObjectNameNotFound;
    fill_network_open(information, *info);
    return kStatusSuccess;
}

bool matches_mask(const std::string& name, const std::string& mask) {
    // '*' any run, '?' one character, case-insensitive; "*.*" matches names without a dot too.
    if (mask.empty() || mask == "*" || mask == "*.*") return true;
    const std::string n = lower(name);
    const std::string m = lower(mask);
    size_t ni = 0, mi = 0, star = std::string::npos, mark = 0;
    while (ni < n.size()) {
        if (mi < m.size() && (m[mi] == '?' || m[mi] == n[ni])) {
            ++ni;
            ++mi;
        } else if (mi < m.size() && m[mi] == '*') {
            star = mi++;
            mark = ni;
        } else if (star != std::string::npos) {
            mi = star + 1;
            ni = ++mark;
        } else {
            return false;
        }
    }
    while (mi < m.size() && m[mi] == '*') ++mi;
    return mi == m.size();
}

void build_listing(FileObject& file, const std::string& mask) {
    file.listing.clear();
    file.next_item = 0;
    file.listed = true;
    if (file.kind == FileObject::Kind::DiscDirectory) {
        for (const XdvdfsImage::Entry& entry : g_disc->list(file.disc_entry)) {
            if (matches_mask(entry.name, mask)) file.listing.push_back({entry.name, disc_info(entry)});
        }
    } else if (file.kind == FileObject::Kind::HostDirectory) {
        std::error_code error;
        for (const auto& item : fs::directory_iterator(file.host_path, error)) {
            const std::u8string name8 = item.path().filename().u8string();
            const std::string name(name8.begin(), name8.end());
            if (!matches_mask(name, mask)) continue;
            if (const std::optional<FileInfo> info = host_info(item.path())) file.listing.push_back({name, *info});
        }
    }
}

NTSTATUS __stdcall NtQueryDirectoryFile(uint32_t handle, uint32_t event, uint32_t apc_routine, uint32_t apc_context,
                                        XIoStatusBlock* io_status, void* information, uint32_t length,
                                        uint32_t information_class, const XAnsiString* file_name, uint8_t restart_scan) {
    const std::shared_ptr<FileObject> file = file_from_handle(handle);
    if (!file) return kStatusInvalidHandle;
    if (information_class != kFileDirectoryInformationClass) {
        log_line("NtQueryDirectoryFile: class %u not implemented", information_class);
        return kStatusInvalidParameter;
    }
    std::lock_guard<std::mutex> lock(file->mutex);
    const bool first = !file->listed || restart_scan;
    if (first) build_listing(*file, ansi_string(file_name));

    NTSTATUS status = kStatusSuccess;
    uint32_t size = 0;
    if (file->next_item >= file->listing.size()) {
        status = first ? kStatusNoSuchFile : kStatusNoMoreFiles;
    } else {
        const DirectoryItem& item = file->listing[file->next_item];
        size = static_cast<uint32_t>(offsetof(XFileDirectoryInformation, file_name) + item.name.size());
        if (length < size) {
            status = kStatusBufferTooSmall;
            size = 0;
        } else {
            auto* out = static_cast<XFileDirectoryInformation*>(information);
            out->next_entry_offset = 0;
            out->file_index = static_cast<uint32_t>(file->next_item);
            out->creation_time = item.info.creation_time;
            out->last_access_time = item.info.last_access_time;
            out->last_write_time = item.info.last_write_time;
            out->change_time = item.info.change_time;
            out->end_of_file = item.info.end_of_file;
            out->allocation_size = item.info.allocation_size;
            out->file_attributes = item.info.attributes;
            out->file_name_length = static_cast<uint32_t>(item.name.size());
            std::memcpy(out->file_name, item.name.data(), item.name.size());
            ++file->next_item;
        }
    }
    complete(io_status, status, size);
    finish_request(event, apc_routine, apc_context, io_status);
    return status;
}

NTSTATUS __stdcall NtDeleteFile(const XObjectAttributes* attributes) {
    const std::optional<Location> location = resolve_attributes(attributes);
    if (!location) return kStatusObjectPathNotFound;
    if (location->device != Device::Partition) return kStatusCannotDelete;
    const fs::path path = host_path_of(*location);
    const BOOL done = fs::is_directory(path) ? RemoveDirectoryW(path.c_str()) : DeleteFileW(path.c_str());
    return done ? kStatusSuccess : status_from_win32(GetLastError());
}

NTSTATUS __stdcall NtFlushBuffersFile(uint32_t handle, XIoStatusBlock* io_status) {
    const std::shared_ptr<FileObject> file = file_from_handle(handle);
    if (!file) return kStatusInvalidHandle;
    if (file->host != INVALID_HANDLE_VALUE) FlushFileBuffers(file->host);
    complete(io_status, kStatusSuccess, 0);
    return kStatusSuccess;
}

NTSTATUS __stdcall NtFsControlFile(uint32_t handle, uint32_t event, uint32_t apc_routine, uint32_t apc_context,
                                   XIoStatusBlock* io_status, uint32_t control_code, void*, uint32_t, void* output,
                                   uint32_t output_length) {
    log_line("NtFsControlFile(handle %08X, code %08X): accepted", handle, control_code);
    if (output && output_length) std::memset(output, 0, output_length);
    complete(io_status, kStatusSuccess, 0);
    finish_request(event, apc_routine, apc_context, io_status);
    return kStatusSuccess;
}

// Sizes of the retail Xbox hard disk layout (bytes).
uint64_t partition_length(uint32_t partition) {
    switch (partition) {
        case 0: return 8ull * 1024 * 1024 * 1024;  // whole disk
        case 1: return 0x1312D6000ull;              // E: user data
        case 2: return 0x1F400000ull;               // C: system
        default: return 0x2EE00000ull;              // X: Y: Z: caches, F: G: extended
    }
}

#pragma pack(push, 1)
struct XDiskGeometry {
    int64_t cylinders;
    uint32_t media_type;
    uint32_t tracks_per_cylinder;
    uint32_t sectors_per_track;
    uint32_t bytes_per_sector;
};
struct XPartitionInformation {
    int64_t starting_offset;
    int64_t partition_length;
    uint32_t hidden_sectors;
    uint32_t partition_number;
    uint8_t partition_type;
    uint8_t boot_indicator;
    uint8_t recognized_partition;
    uint8_t rewrite_partition;
};
#pragma pack(pop)

NTSTATUS __stdcall NtDeviceIoControlFile(uint32_t handle, uint32_t event, uint32_t apc_routine, uint32_t apc_context,
                                         XIoStatusBlock* io_status, uint32_t control_code, void*, uint32_t,
                                         void* output, uint32_t output_length) {
    constexpr uint32_t kIoctlDiskGetDriveGeometry = 0x00070000;
    constexpr uint32_t kIoctlDiskGetPartitionInfo = 0x00074004;
    constexpr uint32_t kFixedMedia = 12;

    const std::shared_ptr<FileObject> file = file_from_handle(handle);
    if (!file) return kStatusInvalidHandle;
    const uint64_t length = partition_length(file->location.partition);
    if (output && output_length) std::memset(output, 0, output_length);
    uint32_t size = 0;
    if (control_code == kIoctlDiskGetDriveGeometry && output_length >= sizeof(XDiskGeometry)) {
        size = sizeof(XDiskGeometry);
        *static_cast<XDiskGeometry*>(output) =
            XDiskGeometry{static_cast<int64_t>(length / kBytesPerSector), kFixedMedia, 1, 1, kBytesPerSector};
    } else if (control_code == kIoctlDiskGetPartitionInfo && output_length >= sizeof(XPartitionInformation)) {
        size = sizeof(XPartitionInformation);
        *static_cast<XPartitionInformation*>(output) = XPartitionInformation{
            0, static_cast<int64_t>(length), 0, file->location.partition, 0x0E, 0, 1, 0};
    } else {
        log_line("NtDeviceIoControlFile(%s, code %08X): accepted with empty output", file->location.xbox_path.c_str(),
                 control_code);
    }
    complete(io_status, kStatusSuccess, size);
    finish_request(event, apc_routine, apc_context, io_status);
    return kStatusSuccess;
}

NTSTATUS __stdcall IoCreateSymbolicLink(const XAnsiString* link_name, const XAnsiString* device_name) {
    const std::string link = ansi_string(link_name);
    const std::string device = ansi_string(device_name);
    log_line("IoCreateSymbolicLink('%s' -> '%s')", link.c_str(), device.c_str());
    // XAPI recreates D: itself at startup; the latest definition wins.
    std::lock_guard<std::mutex> lock(g_links_mutex);
    g_symbolic_links[lower(link)] = device;
    return kStatusSuccess;
}

NTSTATUS __stdcall IoDeleteSymbolicLink(const XAnsiString* link_name) {
    std::lock_guard<std::mutex> lock(g_links_mutex);
    return g_symbolic_links.erase(lower(ansi_string(link_name))) ? kStatusSuccess : kStatusObjectNameNotFound;
}

NTSTATUS __stdcall NtOpenSymbolicLinkObject(uint32_t* link_handle, const XObjectAttributes* attributes) {
    const std::string name = ansi_string(guest<XAnsiString>(attributes->object_name));
    std::string target;
    {
        std::lock_guard<std::mutex> lock(g_links_mutex);
        const auto it = g_symbolic_links.find(lower(name));
        if (it == g_symbolic_links.end()) {
            log_line("NtOpenSymbolicLinkObject('%s'): not found", name.c_str());
            return kStatusObjectNameNotFound;
        }
        target = it->second;
    }
    HandleEntry entry;
    entry.symbolic_link_target = target;
    *link_handle = create_handle(std::move(entry));
    return kStatusSuccess;
}

NTSTATUS __stdcall NtQuerySymbolicLinkObject(uint32_t link_handle, XAnsiString* link_target, uint32_t* returned_length) {
    const std::optional<HandleEntry> entry = lookup_handle(link_handle);
    if (!entry || entry->symbolic_link_target.empty()) return kStatusInvalidHandle;
    const std::string& target = entry->symbolic_link_target;
    if (returned_length) *returned_length = static_cast<uint32_t>(target.size());
    if (link_target->maximum_length < target.size()) return kStatusBufferTooSmall;
    std::memcpy(guest<char>(link_target->buffer), target.data(), target.size());
    link_target->length = static_cast<uint16_t>(target.size());
    return kStatusSuccess;
}

// Driver objects (XAPI's USB peripheral driver creates some). Xbox DEVICE_OBJECT layout, extension right after.
#pragma pack(push, 1)
struct XDeviceObject {
    int16_t type;
    uint16_t size;
    int32_t reference_count;
    uint32_t driver_object;
    uint32_t mounted_or_self_device;
    uint32_t current_irp;
    uint32_t flags;
    uint32_t device_extension;
    uint8_t device_type;
    uint8_t start_io_flags;
    int8_t stack_size;
    uint8_t delete_pending;
    uint32_t sector_size;
    uint32_t alignment_requirement;
    uint8_t device_queue[20];
    uint8_t device_lock[16];
    uint32_t start_io_key;
};
#pragma pack(pop)

NTSTATUS __stdcall IoCreateDevice(uint32_t driver_object, uint32_t extension_size, const XAnsiString* name,
                                  uint32_t device_type, uint8_t /*exclusive*/, uint32_t* device_object) {
    constexpr int16_t kIoTypeDevice = 3;
    const uint32_t size = static_cast<uint32_t>(sizeof(XDeviceObject)) + extension_size;
    auto* device = static_cast<XDeviceObject*>(_aligned_malloc((size + 15) & ~15u, 16));
    std::memset(device, 0, size);
    device->type = kIoTypeDevice;
    device->size = static_cast<uint16_t>(sizeof(XDeviceObject));
    device->reference_count = 1;
    device->driver_object = driver_object;
    device->mounted_or_self_device = address_of(device);
    device->device_extension = extension_size ? address_of(device + 1) : 0;
    device->device_type = static_cast<uint8_t>(device_type);
    device->stack_size = 1;
    *device_object = address_of(device);
    log_line("IoCreateDevice('%s', type %X, extension %X) = %08X", ansi_string(name).c_str(), device_type,
             extension_size, *device_object);
    return kStatusSuccess;
}

}  // namespace

void configure_file_system(const XdvdfsImage* disc, const wchar_t* host_data_directory) {
    g_disc = disc;
    g_host_root = host_data_directory;
    std::error_code ignored;
    for (int partition = 1; partition <= 7; ++partition) {
        fs::create_directories(g_host_root / ("Partition" + std::to_string(partition)), ignored);
    }
    std::lock_guard<std::mutex> lock(g_links_mutex);
    g_symbolic_links[lower("\\??\\D:")] = "\\Device\\CdRom0";
}

void register_io_functions() {
    register_kernel(190, &NtCreateFile);
    register_kernel(202, &NtOpenFile);
    register_kernel(219, &NtReadFile);
    register_kernel(236, &NtWriteFile);
    register_kernel(211, &NtQueryInformationFile);
    register_kernel(226, &NtSetInformationFile);
    register_kernel(218, &NtQueryVolumeInformationFile);
    register_kernel(210, &NtQueryFullAttributesFile);
    register_kernel(207, &NtQueryDirectoryFile);
    register_kernel(195, &NtDeleteFile);
    register_kernel(198, &NtFlushBuffersFile);
    register_kernel(200, &NtFsControlFile);
    register_kernel(196, &NtDeviceIoControlFile);
    register_kernel(67, &IoCreateSymbolicLink);
    register_kernel(69, &IoDeleteSymbolicLink);
    register_kernel(203, &NtOpenSymbolicLinkObject);
    register_kernel(215, &NtQuerySymbolicLinkObject);
    register_kernel(65, &IoCreateDevice);
}

}  // namespace mc3::pc
