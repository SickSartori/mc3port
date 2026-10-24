#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace mc3::pc {

struct FileObject;

// What an Xbox handle refers to. Dispatcher objects (events, semaphores, threads...) are referenced by the
// address of their Xbox-layout structure, so the same object works through handles and through Ke* calls.
struct HandleEntry {
    uint32_t object = 0;
    std::shared_ptr<FileObject> file;
    std::string symbolic_link_target;  // for handles from NtOpenSymbolicLinkObject
};

uint32_t create_handle(HandleEntry entry);
std::optional<HandleEntry> lookup_handle(uint32_t handle);
bool close_handle(uint32_t handle);

// Handle -> dispatcher object address, including the current-thread pseudo handle. 0 if unknown.
uint32_t object_from_handle(uint32_t handle);

}  // namespace mc3::pc
