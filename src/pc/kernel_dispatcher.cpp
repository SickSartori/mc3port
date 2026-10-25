// Waits and synchronization objects, timers, DPCs, IRQL, critical sections and the system clock.
//
// Objects live in game memory with their Xbox layout (DISPATCHER_HEADER first), so objects the game initialises
// inline work the same as ones created through handles. One process-wide lock guards every object state and
// one condition variable wakes waiters: simple, and the game's wait rate is low.

#include <windows.h>
#include <intrin.h>
#include <malloc.h>
#include <mmsystem.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "pc/handles.h"
#include "pc/kernel_internal.h"
#include "pc/log.h"

namespace mc3::pc {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kKpcrDpcRoutineActive = 0x58;
constexpr int64_t kFileTimeUnitsPerMillisecond = 10000;

std::mutex g_mutex;
std::condition_variable g_object_changed;
std::condition_variable g_timer_changed;
std::condition_variable g_dpc_queued;

std::multimap<Clock::time_point, uint32_t> g_timer_queue;
std::unordered_map<uint32_t, std::multimap<Clock::time_point, uint32_t>::iterator> g_active_timers;
std::deque<uint32_t> g_dpc_queue;

volatile uint32_t g_tick_count = 0;
#pragma pack(push, 1)
struct KSystemTime {
    uint32_t low_part;
    int32_t high1_time;
    int32_t high2_time;
};
#pragma pack(pop)
KSystemTime g_system_time{};
KSystemTime g_interrupt_time{};

XDispatcherHeader* header_of(uint32_t object) { return guest<XDispatcherHeader>(object); }

int64_t current_file_time() {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    return (static_cast<int64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
}

void store_system_time(KSystemTime& time, int64_t value) {
    // Same order the Xbox kernel uses, so readers that compare High1Time and High2Time see a consistent value.
    time.high2_time = static_cast<int32_t>(value >> 32);
    time.low_part = static_cast<uint32_t>(value);
    time.high1_time = static_cast<int32_t>(value >> 32);
}

// The last events set (g_mutex held), for log_long_wait.
struct SetRecord {
    uint32_t event;
    uint32_t thread_id;
    Clock::time_point when;
};
SetRecord g_recent_sets[16] = {};
uint32_t g_set_count = 0;

// MC3_TRACE_WAITS=1: log_long_wait is active (idle worker threads also wait for long, so it is opt-in).
const bool g_trace_waits = GetEnvironmentVariableW(L"MC3_TRACE_WAITS", nullptr, 0) != 0;

void log_long_wait(uint32_t count, const uint32_t* objects) {
    if (!g_trace_waits) return;
    std::string list;
    for (uint32_t i = 0; i < count; ++i) {
        const XDispatcherHeader* header = header_of(objects[i]);
        char entry[64];
        std::snprintf(entry, sizeof(entry), " %08X (type %u, state %d)", objects[i], header->type, header->signal_state);
        list += entry;
    }
    log_line("kernel: thread %lu has been waiting 5 s on%s", GetCurrentThreadId(), list.c_str());
    const Clock::time_point now = Clock::now();
    for (uint32_t i = g_set_count > 16 ? g_set_count - 16 : 0; i < g_set_count; ++i) {
        const SetRecord& r = g_recent_sets[i % 16];
        log_line("  event %08X set by thread %u, %lld ms ago", r.event, r.thread_id,
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now - r.when).count()));
    }
}

// --- wait satisfaction (g_mutex held) ---

bool is_signaled(uint32_t object, uint32_t me) {
    const XDispatcherHeader* header = header_of(object);
    if (header->type == kMutantObject) {
        return header->signal_state > 0 || guest<XKMutant>(object)->owner_thread == me;
    }
    return header->signal_state > 0;
}

void consume(uint32_t object, uint32_t me) {
    XDispatcherHeader* header = header_of(object);
    switch (header->type) {
        case kEventSynchronizationObject:
        case kTimerSynchronizationObject:
            header->signal_state = 0;
            break;
        case kSemaphoreObject:
            --header->signal_state;
            break;
        case kMutantObject:
            --header->signal_state;
            guest<XKMutant>(object)->owner_thread = me;
            break;
        default:
            break;
    }
}

Clock::time_point deadline_from(const int64_t* timeout) {
    const int64_t milliseconds = timeout_to_milliseconds(timeout);
    return Clock::now() + std::chrono::milliseconds(milliseconds);
}

// --- timers and DPCs ---

void queue_dpc_locked(uint32_t dpc_address, uint32_t argument1, uint32_t argument2) {
    XKDpc* dpc = guest<XKDpc>(dpc_address);
    if (dpc->inserted) return;
    dpc->inserted = 1;
    dpc->system_argument1 = argument1;
    dpc->system_argument2 = argument2;
    g_dpc_queue.push_back(dpc_address);
    g_dpc_queued.notify_one();
}

bool cancel_timer_locked(uint32_t timer_address) {
    const auto it = g_active_timers.find(timer_address);
    if (it == g_active_timers.end()) return false;
    g_timer_queue.erase(it->second);
    g_active_timers.erase(it);
    guest<XKTimer>(timer_address)->header.inserted = 0;
    return true;
}

void arm_timer_locked(uint32_t timer_address, Clock::time_point due) {
    g_active_timers[timer_address] = g_timer_queue.emplace(due, timer_address);
    guest<XKTimer>(timer_address)->header.inserted = 1;
    g_timer_changed.notify_one();
}

void timer_thread() {
    std::unique_lock<std::mutex> lock(g_mutex);
    for (;;) {
        if (g_timer_queue.empty()) {
            g_timer_changed.wait(lock);
            continue;
        }
        const Clock::time_point due = g_timer_queue.begin()->first;
        if (Clock::now() < due) {
            g_timer_changed.wait_until(lock, due);
            continue;
        }
        const uint32_t timer_address = g_timer_queue.begin()->second;
        g_timer_queue.erase(g_timer_queue.begin());
        g_active_timers.erase(timer_address);

        XKTimer* timer = guest<XKTimer>(timer_address);
        timer->header.inserted = 0;
        timer->header.signal_state = 1;
        if (timer->period > 0) arm_timer_locked(timer_address, due + std::chrono::milliseconds(timer->period));
        if (timer->dpc) {
            const int64_t now = current_file_time();
            queue_dpc_locked(timer->dpc, static_cast<uint32_t>(now), static_cast<uint32_t>(now >> 32));
        }
        g_object_changed.notify_all();
    }
}

// Runs the queued DPCs on the calling game-code thread. `lock` holds g_mutex on entry and on return.
void run_queued_dpcs_locked(std::unique_lock<std::mutex>& lock) {
    XboxThreadContext* context = current_xbox_thread();
    while (!g_dpc_queue.empty()) {
        const uint32_t dpc_address = g_dpc_queue.front();
        g_dpc_queue.pop_front();
        XKDpc* dpc = guest<XKDpc>(dpc_address);
        dpc->inserted = 0;
        const XKDpc snapshot = *dpc;
        lock.unlock();

        using DeferredRoutine = void(__stdcall*)(uint32_t, uint32_t, uint32_t, uint32_t);
        const uint8_t old_irql = raise_irql(kDispatchLevel);
        context->kpcr[kKpcrDpcRoutineActive] = 1;
        reinterpret_cast<DeferredRoutine>(static_cast<uintptr_t>(snapshot.deferred_routine))(
            dpc_address, snapshot.deferred_context, snapshot.system_argument1, snapshot.system_argument2);
        context->kpcr[kKpcrDpcRoutineActive] = 0;
        lower_irql(old_irql);

        lock.lock();
    }
}

// Timer DPCs (and any DPC queued outside an interrupt) run here.
void dpc_thread(XbeTlsTemplate tls) {
    attach_xbox_thread_context(tls, 0);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    std::unique_lock<std::mutex> lock(g_mutex);
    for (;;) {
        g_dpc_queued.wait(lock, [] { return !g_dpc_queue.empty(); });
        run_queued_dpcs_locked(lock);
    }
}

void clock_thread() {
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    const ULONGLONG start = GetTickCount64();
    LARGE_INTEGER frequency, start_counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start_counter);
    for (;;) {
        g_tick_count = static_cast<uint32_t>(GetTickCount64() - start);
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        const int64_t interrupt_time = (counter.QuadPart - start_counter.QuadPart) * 10000000 / frequency.QuadPart;
        store_system_time(g_interrupt_time, interrupt_time);
        store_system_time(g_system_time, current_file_time());
        Sleep(1);
    }
}

// --- kernel exports ---

int32_t __stdcall KeSetEvent(uint32_t event, int32_t /*increment*/, uint8_t /*wait*/) { return set_event(event); }

NTSTATUS __stdcall KeWaitForSingleObject(uint32_t object, uint32_t /*wait_reason*/, uint8_t /*wait_mode*/,
                                         uint8_t /*alertable*/, const int64_t* timeout) {
    return wait_for_objects(1, &object, false, timeout);
}

NTSTATUS __stdcall KeWaitForMultipleObjects(uint32_t count, const uint32_t* objects, uint32_t wait_type,
                                            uint32_t /*wait_reason*/, uint8_t /*wait_mode*/, uint8_t /*alertable*/,
                                            const int64_t* timeout, void* /*wait_blocks*/) {
    return wait_for_objects(count, objects, wait_type == 0, timeout);
}

uint32_t new_object(size_t size, uint8_t type, int32_t signal_state) {
    auto* memory = static_cast<uint8_t*>(_aligned_malloc(size, 16));
    std::memset(memory, 0, size);
    auto* header = reinterpret_cast<XDispatcherHeader*>(memory);
    header->type = type;
    header->size = static_cast<uint8_t>(size / 4);
    header->signal_state = signal_state;
    header->wait_list_head = {address_of(&header->wait_list_head), address_of(&header->wait_list_head)};
    return address_of(memory);
}

// TODO: dispatcher objects created through handles are never freed; a waiter may still reference one after
// NtClose, so freeing needs reference counting.
NTSTATUS __stdcall NtCreateEvent(uint32_t* handle, const XObjectAttributes* /*attributes*/, uint32_t event_type,
                                 uint8_t initial_state) {
    const uint32_t event = new_object(sizeof(XDispatcherHeader), static_cast<uint8_t>(event_type), initial_state ? 1 : 0);
    *handle = create_handle(HandleEntry{event});
    return kStatusSuccess;
}

NTSTATUS __stdcall NtSetEvent(uint32_t handle, int32_t* previous_state) {
    const uint32_t event = object_from_handle(handle);
    if (!event) return kStatusInvalidHandle;
    const int32_t previous = set_event(event);
    if (previous_state) *previous_state = previous;
    return kStatusSuccess;
}

NTSTATUS __stdcall NtCreateSemaphore(uint32_t* handle, const XObjectAttributes* /*attributes*/, int32_t initial_count,
                                     int32_t maximum_count) {
    const uint32_t semaphore = new_object(sizeof(XKSemaphore), kSemaphoreObject, initial_count);
    guest<XKSemaphore>(semaphore)->limit = maximum_count;
    *handle = create_handle(HandleEntry{semaphore});
    return kStatusSuccess;
}

NTSTATUS __stdcall NtReleaseSemaphore(uint32_t handle, int32_t release_count, int32_t* previous_count) {
    const uint32_t object = object_from_handle(handle);
    if (!object) return kStatusInvalidHandle;
    std::lock_guard<std::mutex> lock(g_mutex);
    XKSemaphore* semaphore = guest<XKSemaphore>(object);
    if (previous_count) *previous_count = semaphore->header.signal_state;
    if (release_count <= 0 || semaphore->header.signal_state + release_count > semaphore->limit) {
        return kStatusSemaphoreLimitExceeded;
    }
    semaphore->header.signal_state += release_count;
    g_object_changed.notify_all();
    return kStatusSuccess;
}

NTSTATUS __stdcall NtCreateMutant(uint32_t* handle, const XObjectAttributes* /*attributes*/, uint8_t initial_owner) {
    const uint32_t object = new_object(sizeof(XKMutant), kMutantObject, initial_owner ? 0 : 1);
    if (initial_owner) guest<XKMutant>(object)->owner_thread = kthread_address(current_xbox_thread());
    *handle = create_handle(HandleEntry{object});
    return kStatusSuccess;
}

NTSTATUS __stdcall NtReleaseMutant(uint32_t handle, int32_t* previous_count) {
    const uint32_t object = object_from_handle(handle);
    if (!object) return kStatusInvalidHandle;
    std::lock_guard<std::mutex> lock(g_mutex);
    XKMutant* mutant = guest<XKMutant>(object);
    if (previous_count) *previous_count = mutant->header.signal_state;
    if (mutant->owner_thread != kthread_address(current_xbox_thread())) return kStatusMutantNotOwned;
    if (++mutant->header.signal_state > 0) {
        mutant->owner_thread = 0;
        g_object_changed.notify_all();
    }
    return kStatusSuccess;
}

NTSTATUS __stdcall NtWaitForSingleObject(uint32_t handle, uint8_t /*alertable*/, const int64_t* timeout) {
    const uint32_t object = waitable_from_handle(handle);
    if (!object) return kStatusInvalidHandle;
    return wait_for_objects(1, &object, false, timeout);
}

NTSTATUS __stdcall NtWaitForSingleObjectEx(uint32_t handle, uint8_t /*wait_mode*/, uint8_t /*alertable*/,
                                           const int64_t* timeout) {
    return NtWaitForSingleObject(handle, 0, timeout);
}

NTSTATUS __stdcall NtWaitForMultipleObjectsEx(uint32_t count, const uint32_t* handles, uint32_t wait_type,
                                              uint8_t /*wait_mode*/, uint8_t /*alertable*/, const int64_t* timeout) {
    uint32_t objects[64];
    if (count == 0 || count > 64) return kStatusInvalidParameter;
    for (uint32_t i = 0; i < count; ++i) {
        objects[i] = waitable_from_handle(handles[i]);
        if (!objects[i]) return kStatusInvalidHandle;
    }
    return wait_for_objects(count, objects, wait_type == 0, timeout);
}

void __stdcall KeInitializeTimerEx(XKTimer* timer, uint32_t timer_type) {
    std::memset(timer, 0, sizeof(*timer));
    timer->header.type = static_cast<uint8_t>(kTimerNotificationObject + timer_type);
    timer->header.size = sizeof(XKTimer) / 4;
    timer->header.wait_list_head = {address_of(&timer->header.wait_list_head),
                                    address_of(&timer->header.wait_list_head)};
}

uint8_t __stdcall KeSetTimerEx(XKTimer* timer, int64_t due_time, int32_t period, uint32_t dpc) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t address = address_of(timer);
    const bool was_inserted = cancel_timer_locked(address);
    timer->header.signal_state = 0;
    timer->due_time = due_time;
    timer->period = period;
    timer->dpc = dpc;
    arm_timer_locked(address, deadline_from(&due_time));
    return was_inserted ? 1 : 0;
}

uint8_t __stdcall KeSetTimer(XKTimer* timer, int64_t due_time, uint32_t dpc) { return KeSetTimerEx(timer, due_time, 0, dpc); }

uint8_t __stdcall KeCancelTimer(XKTimer* timer) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return cancel_timer_locked(address_of(timer)) ? 1 : 0;
}

void __stdcall KeInitializeDpc(XKDpc* dpc, uint32_t routine, uint32_t context) {
    std::memset(dpc, 0, sizeof(*dpc));
    dpc->type = kDpcObject;
    dpc->deferred_routine = routine;
    dpc->deferred_context = context;
}

uint8_t __stdcall KeInsertQueueDpc(XKDpc* dpc, uint32_t argument1, uint32_t argument2) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (dpc->inserted) return 0;
    queue_dpc_locked(address_of(dpc), argument1, argument2);
    return 1;
}

uint8_t __stdcall KeRemoveQueueDpc(XKDpc* dpc) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!dpc->inserted) return 0;
    for (auto it = g_dpc_queue.begin(); it != g_dpc_queue.end(); ++it) {
        if (*it == address_of(dpc)) {
            g_dpc_queue.erase(it);
            break;
        }
    }
    dpc->inserted = 0;
    return 1;
}

uint8_t __stdcall KeRaiseIrqlToDpcLevel() { return raise_irql(kDispatchLevel); }
uint8_t __fastcall KfRaiseIrql(uint8_t new_irql) { return raise_irql(new_irql); }
void __fastcall KfLowerIrql(uint8_t new_irql) { lower_irql(new_irql); }

// Interrupts: the emulated hardware (nv2a.cpp) raises them through trigger_interrupt.
#pragma pack(push, 1)
struct XKInterrupt {
    uint32_t service_routine;
    uint32_t service_context;
    uint32_t bus_interrupt_level;
    uint32_t irql;
    uint8_t connected;
    uint8_t share_vector;
    uint8_t mode;
    uint8_t padding;
    uint32_t service_count;
};
#pragma pack(pop)

constexpr uint32_t kPrimaryVectorBase = 0x30;  // matches HalGetInterruptVector
std::mutex g_interrupt_mutex;
std::map<uint32_t, XKInterrupt*> g_interrupts;  // bus interrupt level -> connected interrupt object

void __stdcall KeInitializeInterrupt(XKInterrupt* interrupt, uint32_t service_routine, uint32_t service_context,
                                     uint32_t vector, uint8_t irql, uint32_t mode, uint8_t share_vector) {
    std::memset(interrupt, 0, sizeof(*interrupt));
    interrupt->service_routine = service_routine;
    interrupt->service_context = service_context;
    interrupt->bus_interrupt_level = vector - kPrimaryVectorBase;
    interrupt->irql = irql;
    interrupt->mode = static_cast<uint8_t>(mode);
    interrupt->share_vector = share_vector;
}

uint8_t __stdcall KeConnectInterrupt(XKInterrupt* interrupt) {
    std::lock_guard<std::mutex> lock(g_interrupt_mutex);
    if (interrupt->connected) return 0;
    interrupt->connected = 1;
    g_interrupts[interrupt->bus_interrupt_level] = interrupt;
    log_line("interrupt connected: level %u, ISR %08X, IRQL %u", interrupt->bus_interrupt_level,
             interrupt->service_routine, interrupt->irql);
    return 1;
}

uint8_t __stdcall KeDisconnectInterrupt(XKInterrupt* interrupt) {
    std::lock_guard<std::mutex> lock(g_interrupt_mutex);
    if (!interrupt->connected) return 0;
    interrupt->connected = 0;
    g_interrupts.erase(interrupt->bus_interrupt_level);
    return 1;
}

uint8_t __stdcall KeSynchronizeExecution(void* /*interrupt*/, uint32_t routine, uint32_t context) {
    const uint8_t old_irql = raise_irql(kDispatchLevel + 1);
    const uint8_t result = reinterpret_cast<uint8_t(__stdcall*)(uint32_t)>(static_cast<uintptr_t>(routine))(context);
    lower_irql(old_irql);
    return result;
}

// Xbox critical sections wait on their embedded synchronization event, exactly like the real kernel.
void __stdcall RtlInitializeCriticalSection(XCriticalSection* section) {
    std::memset(section, 0, sizeof(*section));
    section->synchronization.type = kEventSynchronizationObject;
    section->synchronization.size = sizeof(XDispatcherHeader) / 4;
    section->synchronization.wait_list_head = {address_of(&section->synchronization.wait_list_head),
                                               address_of(&section->synchronization.wait_list_head)};
    section->lock_count = -1;
}

void __stdcall RtlEnterCriticalSection(XCriticalSection* section) {
    const uint32_t me = kthread_address(current_xbox_thread());
    if (_InterlockedIncrement(reinterpret_cast<volatile long*>(&section->lock_count)) == 0) {
        section->owning_thread = me;
        section->recursion_count = 1;
    } else if (section->owning_thread == me) {
        ++section->recursion_count;
    } else {
        section->synchronization.type = kEventSynchronizationObject;  // statically initialised sections may be 0
        const uint32_t event = address_of(&section->synchronization);
        wait_for_objects(1, &event, false, nullptr);
        section->owning_thread = me;
        section->recursion_count = 1;
    }
}

void __stdcall RtlLeaveCriticalSection(XCriticalSection* section) {
    if (--section->recursion_count != 0) {
        _InterlockedDecrement(reinterpret_cast<volatile long*>(&section->lock_count));
        return;
    }
    section->owning_thread = 0;
    if (_InterlockedDecrement(reinterpret_cast<volatile long*>(&section->lock_count)) >= 0) {
        section->synchronization.type = kEventSynchronizationObject;
        set_event(address_of(&section->synchronization));
    }
}

uint8_t __stdcall RtlTryEnterCriticalSection(XCriticalSection* section) {
    const uint32_t me = kthread_address(current_xbox_thread());
    if (_InterlockedCompareExchange(reinterpret_cast<volatile long*>(&section->lock_count), 0, -1) == -1) {
        section->owning_thread = me;
        section->recursion_count = 1;
        return 1;
    }
    if (section->owning_thread == me) {
        _InterlockedIncrement(reinterpret_cast<volatile long*>(&section->lock_count));
        ++section->recursion_count;
        return 1;
    }
    return 0;
}

}  // namespace

NTSTATUS wait_for_objects(uint32_t count, const uint32_t* objects, bool wait_all, const int64_t* timeout) {
    const uint32_t me = kthread_address(current_xbox_thread());
    const bool infinite = timeout == nullptr;
    const Clock::time_point deadline = infinite ? Clock::time_point::max() : deadline_from(timeout);
    const Clock::time_point started = Clock::now();
    bool reported = false;

    std::unique_lock<std::mutex> lock(g_mutex);
    for (;;) {
        if (wait_all) {
            bool all = true;
            for (uint32_t i = 0; i < count && all; ++i) all = is_signaled(objects[i], me);
            if (all) {
                for (uint32_t i = 0; i < count; ++i) consume(objects[i], me);
                return kStatusSuccess;
            }
        } else {
            for (uint32_t i = 0; i < count; ++i) {
                if (is_signaled(objects[i], me)) {
                    consume(objects[i], me);
                    return kStatusWait0 + static_cast<NTSTATUS>(i);
                }
            }
        }
        if (infinite) {
            // A wait that lasts is logged once with what it waits for and the events set lately: hangs show up
            // as "waiting on X, but Y was set".
            g_object_changed.wait_for(lock, std::chrono::seconds(5));
            if (!reported && Clock::now() - started > std::chrono::seconds(5)) {
                reported = true;
                log_long_wait(count, objects);
            }
        } else if (g_object_changed.wait_until(lock, deadline) == std::cv_status::timeout) {
            // One last check happens on the next loop turn only if something changed; a timeout is final.
            bool signaled = false;
            for (uint32_t i = 0; i < count && !signaled; ++i) signaled = is_signaled(objects[i], me);
            if (!signaled) return kStatusTimeout;
        }
    }
}

bool trigger_interrupt(uint32_t bus_interrupt_level) {
    XKInterrupt* interrupt = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_interrupt_mutex);
        const auto it = g_interrupts.find(bus_interrupt_level);
        if (it == g_interrupts.end()) return false;
        interrupt = it->second;
    }
    using ServiceRoutine = uint8_t(__stdcall*)(uint32_t interrupt, uint32_t context);
    const uint8_t old_irql = raise_irql(static_cast<uint8_t>(interrupt->irql));
    ++interrupt->service_count;
    reinterpret_cast<ServiceRoutine>(static_cast<uintptr_t>(interrupt->service_routine))(address_of(interrupt),
                                                                                        interrupt->service_context);
    lower_irql(old_irql);
    // Like a CPU leaving interrupt level: the DPCs the ISR queued run now, before the next interrupt can be taken.
    // Otherwise a second interrupt (the GPU runs in parallel) could overwrite what the ISR saved for its DPC.
    std::unique_lock<std::mutex> lock(g_mutex);
    run_queued_dpcs_locked(lock);
    return true;
}

int32_t set_event(uint32_t event) {
    std::lock_guard<std::mutex> lock(g_mutex);
    XDispatcherHeader* header = header_of(event);
    const int32_t previous = header->signal_state;
    header->signal_state = 1;
    g_recent_sets[g_set_count++ % 16] = {event, GetCurrentThreadId(), Clock::now()};
    g_object_changed.notify_all();
    return previous;
}

void signal_thread_object(uint32_t kthread) {
    std::lock_guard<std::mutex> lock(g_mutex);
    header_of(kthread)->signal_state = 1;
    g_object_changed.notify_all();
}

int64_t timeout_to_milliseconds(const int64_t* timeout) {
    if (!timeout) return -1;
    int64_t units = *timeout;
    if (units >= 0) units = current_file_time() - units;  // absolute: how far in the future, as a negative
    if (units >= 0) return 0;
    return (-units + kFileTimeUnitsPerMillisecond - 1) / kFileTimeUnitsPerMillisecond;
}

void start_dispatcher_services(const XbeTlsTemplate& tls) {
    store_system_time(g_system_time, current_file_time());
    std::thread(clock_thread).detach();
    std::thread(timer_thread).detach();
    start_game_code_thread([tls] { dpc_thread(tls); });
}

void* kernel_tick_count() { return const_cast<uint32_t*>(&g_tick_count); }
void* kernel_system_time() { return &g_system_time; }
void* kernel_interrupt_time() { return &g_interrupt_time; }

void register_dispatcher_functions() {
    register_kernel(145, &KeSetEvent);
    register_kernel(158, &KeWaitForMultipleObjects);
    register_kernel(159, &KeWaitForSingleObject);
    register_kernel(189, &NtCreateEvent);
    register_kernel(192, &NtCreateMutant);
    register_kernel(193, &NtCreateSemaphore);
    register_kernel(221, &NtReleaseMutant);
    register_kernel(222, &NtReleaseSemaphore);
    register_kernel(225, &NtSetEvent);
    register_kernel(233, &NtWaitForSingleObject);
    register_kernel(234, &NtWaitForSingleObjectEx);
    register_kernel(235, &NtWaitForMultipleObjectsEx);
    register_kernel(113, &KeInitializeTimerEx);
    register_kernel(149, &KeSetTimer);
    register_kernel(150, &KeSetTimerEx);
    register_kernel(97, &KeCancelTimer);
    register_kernel(107, &KeInitializeDpc);
    register_kernel(119, &KeInsertQueueDpc);
    register_kernel(137, &KeRemoveQueueDpc);
    register_kernel(129, &KeRaiseIrqlToDpcLevel);
    register_kernel(160, &KfRaiseIrql);
    register_kernel(161, &KfLowerIrql);
    register_kernel(109, &KeInitializeInterrupt);
    register_kernel(98, &KeConnectInterrupt);
    register_kernel(100, &KeDisconnectInterrupt);
    register_kernel(153, &KeSynchronizeExecution);
    register_kernel(291, &RtlInitializeCriticalSection);
    register_kernel(277, &RtlEnterCriticalSection);
    register_kernel(294, &RtlLeaveCriticalSection);
    register_kernel(306, &RtlTryEnterCriticalSection);
}

}  // namespace mc3::pc
