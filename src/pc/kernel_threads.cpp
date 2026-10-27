// Threads: creation, termination, suspension, priority, sleeping and object references.

#include <windows.h>

#include <algorithm>

#include "pc/game_pause.h"
#include "pc/handles.h"
#include "pc/kernel_internal.h"
#include "pc/log.h"

namespace mc3::pc {
namespace {

// Xbox stacks are committed up front and the Xbox CRT does not probe large stack frames page by page. On Windows
// that would jump over the guard page into uncommitted memory, where not even an exception can be delivered, so
// game threads get their whole stack committed.
constexpr SIZE_T kMinimumStackSize = 1024 * 1024;

struct ThreadStart {
    uint32_t start_routine;
    uint32_t start_context;
    uint32_t system_routine;
    uint32_t tls_area_size;
    HANDLE ready;  // set once the Xbox context exists
    HANDLE go;     // set by the creator once the handle is published
    XboxThreadContext* context;
};

[[noreturn]] void terminate_current_thread(uint32_t exit_status) {
    if (XboxThreadContext* context = current_xbox_thread()) signal_thread_object(kthread_address(context));
    ExitThread(exit_status);
}

DWORD WINAPI xbox_thread_main(void* parameter) {
    auto* start = static_cast<ThreadStart*>(parameter);
    start->context = attach_xbox_thread_context(XbeTlsTemplate{}, start->tls_area_size);
    SetEvent(start->ready);
    WaitForSingleObject(start->go, INFINITE);

    const ThreadStart copy = *start;
    CloseHandle(start->ready);
    CloseHandle(start->go);
    delete start;

    // XAPI passes its own startup routine as SystemRoutine; it sets up TLS and calls StartRoutine itself.
    if (copy.system_routine) {
        reinterpret_cast<void(__stdcall*)(uint32_t, uint32_t)>(static_cast<uintptr_t>(copy.system_routine))(
            copy.start_routine, copy.start_context);
    } else {
        reinterpret_cast<void(__stdcall*)(uint32_t)>(static_cast<uintptr_t>(copy.start_routine))(copy.start_context);
    }
    terminate_current_thread(0);
}

NTSTATUS __stdcall PsCreateSystemThreadEx(uint32_t* thread_handle, uint32_t /*thread_extension_size*/,
                                          uint32_t kernel_stack_size, uint32_t tls_data_size, uint32_t* thread_id,
                                          uint32_t start_routine, uint32_t start_context, uint8_t create_suspended,
                                          uint8_t /*debugger_thread*/, uint32_t system_routine) {
    auto* start = new ThreadStart{start_routine,
                                  start_context,
                                  system_routine,
                                  tls_data_size,
                                  CreateEventW(nullptr, TRUE, FALSE, nullptr),
                                  CreateEventW(nullptr, TRUE, FALSE, nullptr),
                                  nullptr};
    const SIZE_T stack = std::max<SIZE_T>(kernel_stack_size, kMinimumStackSize);
    HANDLE thread = CreateThread(nullptr, stack, xbox_thread_main, start, 0, nullptr);  // size = committed
    if (!thread) {
        log_line("PsCreateSystemThreadEx: CreateThread failed (error %lu)", GetLastError());
        CloseHandle(start->ready);
        CloseHandle(start->go);
        delete start;
        return kStatusNoMemory;
    }
    WaitForSingleObject(start->ready, INFINITE);
    XboxThreadContext* context = start->context;
    CloseHandle(thread);  // the context keeps its own handle

    if (thread_handle) *thread_handle = create_handle(HandleEntry{kthread_address(context)});
    if (thread_id) *thread_id = context->thread_id;
    log_line("thread %u created: start %08X context %08X%s", context->thread_id, start_routine, start_context,
             create_suspended ? " (suspended)" : "");
    if (create_suspended) SuspendThread(context->windows_thread);
    SetEvent(start->go);
    return kStatusSuccess;
}

void __stdcall PsTerminateSystemThread(uint32_t exit_status) { terminate_current_thread(exit_status); }

XboxThreadContext* thread_from_handle(uint32_t handle) { return xbox_thread_from_kthread(object_from_handle(handle)); }

NTSTATUS __stdcall NtSuspendThread(uint32_t handle, uint32_t* previous_suspend_count) {
    XboxThreadContext* context = thread_from_handle(handle);
    if (!context) return kStatusInvalidHandle;
    const DWORD previous = SuspendThread(context->windows_thread);
    if (previous_suspend_count) *previous_suspend_count = previous;
    return previous == static_cast<DWORD>(-1) ? kStatusUnsuccessful : kStatusSuccess;
}

NTSTATUS __stdcall NtResumeThread(uint32_t handle, uint32_t* previous_suspend_count) {
    XboxThreadContext* context = thread_from_handle(handle);
    if (!context) return kStatusInvalidHandle;
    const DWORD previous = ResumeThread(context->windows_thread);
    if (previous_suspend_count) *previous_suspend_count = previous;
    return previous == static_cast<DWORD>(-1) ? kStatusUnsuccessful : kStatusSuccess;
}

// Xbox priority increments: -2..2 like Win32, with +-16 meaning time critical / idle.
int32_t __stdcall KeSetBasePriorityThread(uint32_t kthread, int32_t increment) {
    XboxThreadContext* context = xbox_thread_from_kthread(kthread);
    if (!context) return 0;
    const int32_t previous = context->base_priority_increment;
    context->base_priority_increment = increment;
    int priority = THREAD_PRIORITY_NORMAL;
    if (increment >= 15) {
        priority = THREAD_PRIORITY_TIME_CRITICAL;
    } else if (increment <= -15) {
        priority = THREAD_PRIORITY_IDLE;
    } else {
        priority = std::clamp(increment, -2, 2);
    }
    SetThreadPriority(context->windows_thread, priority);
    return previous;
}

NTSTATUS __stdcall KeDelayExecutionThread(uint8_t /*wait_mode*/, uint8_t /*alertable*/, const int64_t* interval) {
    game_pause_point();
    const int64_t milliseconds = timeout_to_milliseconds(interval);
    Sleep(milliseconds < 0 ? INFINITE : static_cast<DWORD>(std::min<int64_t>(milliseconds, 0x7FFFFFFF)));
    game_pause_point();
    return kStatusSuccess;
}

NTSTATUS __stdcall NtYieldExecution() {
    constexpr NTSTATUS kStatusNoYieldPerformed = 0x40000024;
    game_pause_point();
    return SwitchToThread() ? kStatusSuccess : kStatusNoYieldPerformed;
}

NTSTATUS __stdcall NtClose(uint32_t handle) { return close_handle(handle) ? kStatusSuccess : kStatusInvalidHandle; }

NTSTATUS __stdcall NtDuplicateObject(uint32_t source_handle, uint32_t* target_handle, uint32_t options) {
    constexpr uint32_t kDuplicateCloseSource = 1;
    std::optional<HandleEntry> entry = lookup_handle(source_handle);
    if (!entry && source_handle == kCurrentThreadHandle) entry = HandleEntry{object_from_handle(source_handle)};
    if (!entry) return kStatusInvalidHandle;
    *target_handle = create_handle(*entry);
    if (options & kDuplicateCloseSource) close_handle(source_handle);
    return kStatusSuccess;
}

NTSTATUS __stdcall ObReferenceObjectByHandle(uint32_t handle, uint32_t /*object_type*/, uint32_t* object) {
    const uint32_t address = object_from_handle(handle);
    if (!address) {
        log_line("ObReferenceObjectByHandle: handle %08X is not a dispatcher object", handle);
        return kStatusInvalidHandle;
    }
    *object = address;
    return kStatusSuccess;
}

void __fastcall ObfDereferenceObject(uint32_t /*object*/) {}

}  // namespace

void register_thread_functions() {
    register_kernel(255, &PsCreateSystemThreadEx);
    register_kernel(258, &PsTerminateSystemThread);
    register_kernel(231, &NtSuspendThread);
    register_kernel(224, &NtResumeThread);
    register_kernel(143, &KeSetBasePriorityThread);
    register_kernel(99, &KeDelayExecutionThread);
    register_kernel(238, &NtYieldExecution);
    register_kernel(187, &NtClose);
    register_kernel(197, &NtDuplicateObject);
    register_kernel(246, &ObReferenceObjectByHandle);
    register_kernel(250, &ObfDereferenceObject);
}

}  // namespace mc3::pc
