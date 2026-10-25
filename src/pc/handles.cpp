#include "pc/handles.h"

#include <mutex>
#include <unordered_map>

#include "pc/xbox_thread.h"
#include "pc/xbox_types.h"

namespace mc3::pc {
namespace {

constexpr uint32_t kFirstHandle = 0x1000;
constexpr uint32_t kHandleStep = 4;  // Xbox handles are multiples of 4, like NT ones

std::mutex g_mutex;
std::unordered_map<uint32_t, HandleEntry> g_handles;
uint32_t g_next_handle = kFirstHandle;

}  // namespace

uint32_t create_handle(HandleEntry entry) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t handle = g_next_handle;
    g_next_handle += kHandleStep;
    g_handles.emplace(handle, std::move(entry));
    return handle;
}

std::optional<HandleEntry> lookup_handle(uint32_t handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_handles.find(handle);
    if (it == g_handles.end()) return std::nullopt;
    return it->second;
}

bool close_handle(uint32_t handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_handles.erase(handle) != 0;
}

uint32_t object_from_handle(uint32_t handle) {
    if (handle == kCurrentThreadHandle) return kthread_address(current_xbox_thread());
    const std::optional<HandleEntry> entry = lookup_handle(handle);
    return entry ? entry->object : 0;
}

uint32_t waitable_from_handle(uint32_t handle) {
    if (handle == kCurrentThreadHandle) return kthread_address(current_xbox_thread());
    const std::optional<HandleEntry> entry = lookup_handle(handle);
    if (!entry) return 0;
    return entry->wait_object ? entry->wait_object : entry->object;
}

}  // namespace mc3::pc
