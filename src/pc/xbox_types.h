#pragma once

// Xbox kernel ABI types as the game sees them (32-bit, little-endian, packed like the XDK headers).

#include <cstdint>

namespace mc3::pc {

using NTSTATUS = int32_t;

constexpr NTSTATUS kStatusSuccess = 0;
constexpr NTSTATUS kStatusWait0 = 0;
constexpr NTSTATUS kStatusAbandonedWait0 = 0x80;
constexpr NTSTATUS kStatusUserApc = 0xC0;
constexpr NTSTATUS kStatusTimeout = 0x102;
constexpr NTSTATUS kStatusPending = 0x103;
constexpr NTSTATUS kStatusNoMoreFiles = static_cast<NTSTATUS>(0x80000006);
constexpr NTSTATUS kStatusUnsuccessful = static_cast<NTSTATUS>(0xC0000001);
constexpr NTSTATUS kStatusNotImplemented = static_cast<NTSTATUS>(0xC0000002);
constexpr NTSTATUS kStatusInvalidHandle = static_cast<NTSTATUS>(0xC0000008);
constexpr NTSTATUS kStatusInvalidParameter = static_cast<NTSTATUS>(0xC000000D);
constexpr NTSTATUS kStatusNoSuchFile = static_cast<NTSTATUS>(0xC000000F);
constexpr NTSTATUS kStatusEndOfFile = static_cast<NTSTATUS>(0xC0000011);
constexpr NTSTATUS kStatusNoMemory = static_cast<NTSTATUS>(0xC0000017);
constexpr NTSTATUS kStatusAccessDenied = static_cast<NTSTATUS>(0xC0000022);
constexpr NTSTATUS kStatusBufferTooSmall = static_cast<NTSTATUS>(0xC0000023);
constexpr NTSTATUS kStatusObjectNameInvalid = static_cast<NTSTATUS>(0xC0000033);
constexpr NTSTATUS kStatusObjectNameNotFound = static_cast<NTSTATUS>(0xC0000034);
constexpr NTSTATUS kStatusObjectNameCollision = static_cast<NTSTATUS>(0xC0000035);
constexpr NTSTATUS kStatusObjectPathNotFound = static_cast<NTSTATUS>(0xC000003A);
constexpr NTSTATUS kStatusMutantNotOwned = static_cast<NTSTATUS>(0xC0000046);
constexpr NTSTATUS kStatusSemaphoreLimitExceeded = static_cast<NTSTATUS>(0xC0000047);
constexpr NTSTATUS kStatusDiskFull = static_cast<NTSTATUS>(0xC000007F);
constexpr NTSTATUS kStatusFileIsADirectory = static_cast<NTSTATUS>(0xC00000BA);
constexpr NTSTATUS kStatusNotADirectory = static_cast<NTSTATUS>(0xC0000103);
constexpr NTSTATUS kStatusDirectoryNotEmpty = static_cast<NTSTATUS>(0xC0000101);
constexpr NTSTATUS kStatusCannotDelete = static_cast<NTSTATUS>(0xC0000121);

constexpr uint32_t kCurrentProcessHandle = 0xFFFFFFFF;
constexpr uint32_t kCurrentThreadHandle = 0xFFFFFFFE;

// Dispatcher object types (DISPATCHER_HEADER.Type).
enum : uint8_t {
    kEventNotificationObject = 0,
    kEventSynchronizationObject = 1,
    kMutantObject = 2,
    kSemaphoreObject = 5,
    kThreadObject = 6,
    kTimerNotificationObject = 8,
    kTimerSynchronizationObject = 9,
    kDpcObject = 19,
};

#pragma pack(push, 1)

struct XListEntry {
    uint32_t flink;
    uint32_t blink;
};

struct XDispatcherHeader {
    uint8_t type;
    uint8_t absolute;
    uint8_t size;  // in dwords
    uint8_t inserted;
    int32_t signal_state;
    XListEntry wait_list_head;
};
static_assert(sizeof(XDispatcherHeader) == 16);

struct XKSemaphore {
    XDispatcherHeader header;
    int32_t limit;
};

struct XKMutant {
    XDispatcherHeader header;
    XListEntry mutant_list_entry;
    uint32_t owner_thread;
    uint8_t abandoned;
    uint8_t padding[3];
};

struct XKTimer {
    XDispatcherHeader header;
    int64_t due_time;
    XListEntry timer_list_entry;
    uint32_t dpc;
    int32_t period;
};
static_assert(sizeof(XKTimer) == 40);

struct XKDpc {
    int16_t type;
    uint8_t inserted;
    uint8_t padding;
    XListEntry dpc_list_entry;
    uint32_t deferred_routine;
    uint32_t deferred_context;
    uint32_t system_argument1;
    uint32_t system_argument2;
};
static_assert(sizeof(XKDpc) == 28);

// Xbox RTL_CRITICAL_SECTION: a synchronization event header followed by the lock state.
struct XCriticalSection {
    XDispatcherHeader synchronization;
    int32_t lock_count;
    int32_t recursion_count;
    uint32_t owning_thread;
};
static_assert(sizeof(XCriticalSection) == 28);

struct XAnsiString {
    uint16_t length;
    uint16_t maximum_length;
    uint32_t buffer;
};

struct XObjectAttributes {
    uint32_t root_directory;
    uint32_t object_name;  // XAnsiString*
    uint32_t attributes;
};

struct XIoStatusBlock {
    NTSTATUS status;
    uint32_t information;
};

#pragma pack(pop)

template <typename T>
T* guest(uint32_t address) {
    return reinterpret_cast<T*>(static_cast<uintptr_t>(address));
}

}  // namespace mc3::pc
