#include "pc/xbox_thread.h"

#include <windows.h>
#include <dbghelp.h>
#include <intrin.h>
#include <malloc.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/byteorder.h"
#include "pc/exec_memory.h"
#include "pc/log.h"
#include "pc/profile.h"
#include "pc/xbox_types.h"

namespace mc3::pc {
namespace {

// Xbox KPCR layout (the fields the game reads, see docs/ANALISI_XBE.md).
constexpr uint32_t kKpcrStackBase = 0x04;   // NtTib.StackBase: on Xbox it points just past the TLS area
constexpr uint32_t kKpcrStackLimit = 0x08;  // NtTib.StackLimit
constexpr uint32_t kKpcrSelf = 0x18;        // NtTib.Self
constexpr uint32_t kKpcrSelfPcr = 0x1C;
constexpr uint32_t kKpcrPrcb = 0x20;
constexpr uint32_t kKpcrIrql = 0x24;
constexpr uint32_t kKpcrPrcbData = 0x28;  // KPRCB starts here; its first field is CurrentThread

// Xbox KTHREAD/ETHREAD layout.
constexpr uint32_t kKThreadStackBase = 0x1C;
constexpr uint32_t kKThreadStackLimit = 0x20;
constexpr uint32_t kKThreadTlsData = 0x28;
constexpr uint32_t kKThreadPriority = 0x32;
constexpr uint32_t kKThreadApcStateProcess = 0x44;
constexpr uint32_t kEThreadUniqueThread = 0x12C;

// Xbox KPROCESS layout.
constexpr uint32_t kKProcessThreadQuantum = 0x14;
constexpr uint32_t kKProcessBasePriority = 0x18;
constexpr uint32_t kKProcessSize = 0x40;

constexpr uint32_t kTebStackLimit = 0x08;
constexpr uint8_t kNormalBasePriority = 8;

thread_local XboxThreadContext* t_context = nullptr;
std::mutex g_threads_mutex;
std::vector<XboxThreadContext*> g_threads;
std::recursive_mutex g_dispatch_level_lock;

alignas(16) uint8_t g_process[kKProcessSize] = {};
std::once_flag g_process_once;

}  // namespace

DWORD_PTR g_game_cores = 0;  // 0: no restriction

XboxThreadContext* attach_xbox_thread_context(const XbeTlsTemplate& tls, uint32_t tls_area_size) {
    std::call_once(g_process_once, [] {
        store_le32(g_process + kKProcessThreadQuantum, 60);  // 20 ms, the XAPI default
        g_process[kKProcessBasePriority] = kNormalBasePriority;
    });
    if (g_game_cores) SetThreadAffinityMask(GetCurrentThread(), g_game_cores);
    // No dynamic boosts (input, foreground window, wake-ups): one could lift a normal game thread to the interrupt
    // thread's priority, and Windows would then take turns between them while the interrupt is at DISPATCH_LEVEL.
    SetThreadPriorityBoost(GetCurrentThread(), TRUE);

    auto* context = new XboxThreadContext{};
    // The TLS area sits right below the thread's StackBase; XAPI sets the TLS index to -(area size / 4), so
    // [StackBase + index * 4] is the area's first dword, which points at the TLS data that follows it.
    const size_t tls_size =
        std::max<size_t>(tls_area_size, (4 + tls.data_size + tls.zero_fill_size + 3) & ~size_t{3});
    context->tls_area = static_cast<uint8_t*>(_aligned_malloc((tls_size + 15) & ~size_t{15}, 16));
    std::memset(context->tls_area, 0, tls_size);
    store_le32(context->tls_area, address_of(context->tls_area + 4));
    if (tls.data_size) std::memcpy(context->tls_area + 4, tls.data, tls.data_size);

    const uint32_t stack_limit = __readfsdword(kTebStackLimit);
    uint8_t* kpcr = context->kpcr;
    const uint32_t stack_base = address_of(context->tls_area + tls_size);
    store_le32(kpcr + kKpcrStackBase, stack_base);
    store_le32(kpcr + kKpcrStackLimit, stack_limit);
    store_le32(kpcr + kKpcrSelf, address_of(kpcr));
    store_le32(kpcr + kKpcrSelfPcr, address_of(kpcr));
    store_le32(kpcr + kKpcrPrcb, address_of(kpcr + kKpcrPrcbData));
    kpcr[kKpcrIrql] = kPassiveLevel;
    store_le32(kpcr + kKpcrPrcbData, address_of(context->kthread));

    uint8_t* kthread = context->kthread;
    auto* header = reinterpret_cast<XDispatcherHeader*>(kthread);
    header->type = kThreadObject;
    header->size = static_cast<uint8_t>(std::min<uint32_t>(kXboxKThreadSize / 4, 0xFF));
    header->wait_list_head = {address_of(&header->wait_list_head), address_of(&header->wait_list_head)};
    store_le32(kthread + kKThreadStackBase, stack_base);
    store_le32(kthread + kKThreadStackLimit, stack_limit);
    store_le32(kthread + kKThreadTlsData, address_of(context->tls_area));
    kthread[kKThreadPriority] = kNormalBasePriority;
    store_le32(kthread + kKThreadApcStateProcess, address_of(g_process));

    context->thread_id = GetCurrentThreadId();
    store_le32(kthread + kEThreadUniqueThread, context->thread_id);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &context->windows_thread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);

    t_context = context;
    __writefsdword(kXboxContextFsSlot, address_of(kpcr));
    profile_name_thread(true, "thread %u", context->thread_id);
    {
        std::lock_guard<std::mutex> lock(g_threads_mutex);
        g_threads.push_back(context);
    }
    return context;
}

XboxThreadContext* current_xbox_thread() { return t_context; }

XboxThreadContext* xbox_thread_from_kthread(uint32_t kthread_address) {
    if (!kthread_address) return nullptr;
    return reinterpret_cast<XboxThreadContext*>(static_cast<uintptr_t>(kthread_address) -
                                                offsetof(XboxThreadContext, kthread));
}

uint32_t kthread_address(const XboxThreadContext* context) { return context ? address_of(context->kthread) : 0; }

uint8_t current_irql() { return t_context ? t_context->kpcr[kKpcrIrql] : kPassiveLevel; }

std::atomic<DWORD> g_dispatch_owner{0};  // the Windows thread at DISPATCH_LEVEL or above, 0 if none
constexpr int kNoRestore = THREAD_PRIORITY_ERROR_RETURN;

// A game thread below the interrupt thread's priority is raised to it, and gets back the returned value later; the
// interrupt and DPC threads are there already (kNoRestore).
int raise_to_interrupt_priority() {
    if (!t_context) return kNoRestore;
    const int priority = GetThreadPriority(GetCurrentThread());
    if (priority == THREAD_PRIORITY_ERROR_RETURN || priority >= THREAD_PRIORITY_TIME_CRITICAL) return kNoRestore;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    return priority;
}

void restore_priority(int priority) {
    if (priority != kNoRestore) SetThreadPriority(GetCurrentThread(), priority);
}

uint8_t raise_irql(uint8_t new_irql) {
    const uint8_t old_irql = current_irql();
    if (old_irql < kDispatchLevel && new_irql >= kDispatchLevel) {
        // Like the Xbox's processor at DISPATCH_LEVEL: no other game thread may run until the level comes down, so
        // the thread runs at the interrupt thread's priority meanwhile (see GameLock).
        const int restore = raise_to_interrupt_priority();
        if (!g_dispatch_level_lock.try_lock()) {
            ProfileScope scope(ProfileTime::dispatch_lock);
            g_dispatch_level_lock.lock();
        }
        g_dispatch_owner = GetCurrentThreadId();
        if (t_context) t_context->dispatch_restore_priority = restore;
    }
    if (t_context) t_context->kpcr[kKpcrIrql] = new_irql;
    return old_irql;
}

void lower_irql(uint8_t new_irql) {
    const uint8_t old_irql = current_irql();
    if (t_context) t_context->kpcr[kKpcrIrql] = new_irql;
    if (old_irql >= kDispatchLevel && new_irql < kDispatchLevel) {
        g_dispatch_owner = 0;
        g_dispatch_level_lock.unlock();
        if (t_context) restore_priority(std::exchange(t_context->dispatch_restore_priority, kNoRestore));
    }
}

// At DISPATCH_LEVEL a busy lock is spun on, as the Xbox's kernel does, not waited for: waiting would leave the game's
// core to the other game threads, which on the Xbox cannot run until this one comes down. The holder is then a host
// thread on another core (the mixer, the timer thread) and lets go soon; one that does not within 2 ms (a game
// thread on the game's core, which cannot run while this one spins) is waited for after all.
void lock_at_irql(std::unique_lock<std::mutex>& lock) {
    if (lock.try_lock()) return;
    if (current_irql() < kDispatchLevel) {
        lock.lock();
        return;
    }
    static const uint64_t kSpinLimit = [] {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        return static_cast<uint64_t>(frequency.QuadPart / 500);
    }();
    const uint64_t start = profile_ticks();
    for (uint32_t spins = 1;; ++spins) {
        _mm_pause();
        if (lock.try_lock()) {
            profile_dispatch_spin(profile_ticks() - start, false);
            return;
        }
        if (spins % 64 == 0 && profile_ticks() - start > kSpinLimit) break;
    }
    lock.lock();
    profile_dispatch_spin(profile_ticks() - start, true);
}

GameLock::GameLock(std::mutex& mutex) : restore_(raise_to_interrupt_priority()), lock_(mutex, std::defer_lock) {
    lock_at_irql(lock_);
}

GameLock::~GameLock() {
    if (lock_.owns_lock()) lock_.unlock();
    restore_priority(restore_);
}

uint32_t another_thread_at_dispatch() {
    if (!t_context) return 0;  // host threads (mixer, GPU) are not the Xbox's processor
    const DWORD owner = g_dispatch_owner;
    return owner && owner != GetCurrentThreadId() && current_irql() < kDispatchLevel ? owner : 0;
}

uint8_t* xbox_process() { return g_process; }

void set_game_cores(unsigned long mask) { g_game_cores = mask; }

unsigned long emulation_cores() {
    DWORD_PTR process_mask = 0, system_mask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) return 0;
    const DWORD_PTR others = process_mask & ~g_game_cores;
    return g_game_cores && others ? others : 0;
}

void start_game_code_thread(std::function<void()> body) {
    constexpr SIZE_T kStackSize = 1024 * 1024;
    auto* owned = new std::function<void()>(std::move(body));
    HANDLE thread = CreateThread(
        nullptr, kStackSize,
        [](void* parameter) -> DWORD {
            std::unique_ptr<std::function<void()>> function(static_cast<std::function<void()>*>(parameter));
            (*function)();
            return 0;
        },
        owned, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        delete owned;
    }
}

uint32_t g_code_begin = 0, g_code_end = 0;

// A copy of a suspended thread's stack, walked after the thread is resumed (walking while it is suspended could
// block on a lock it holds, such as the heap's).
struct StackCopy {
    uint32_t base = 0;
    std::vector<uint8_t> bytes;
};
// Names (from the PDB) of the values on the copied stack that point into this DLL: the host functions the thread
// is in. A heuristic like the game scan above (stale values can appear), but it needs no unwind information.
std::string describe_frames(const CONTEXT& /*registers*/, const StackCopy& copy) {
    static bool symbols = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
    static const auto [module_begin, module_end] = [] {
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&g_code_begin), &module);  // any address in this DLL
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);
        const uintptr_t begin = reinterpret_cast<uintptr_t>(module);
        return std::pair<uintptr_t, uintptr_t>{begin, begin + nt->OptionalHeader.SizeOfImage};
    }();
    std::string text;
    int found = 0;
    for (size_t offset = 0; offset + 4 <= copy.bytes.size() && found < 12; offset += 4) {
        uint32_t value;
        std::memcpy(&value, copy.bytes.data() + offset, 4);
        if (value < module_begin || value >= module_end) continue;
        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 96] = {};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 96;
        DWORD64 displacement = 0;
        if (!symbols || !SymFromAddr(GetCurrentProcess(), value, &displacement, symbol)) continue;
        char entry[160];
        std::snprintf(entry, sizeof(entry), " %s+%llX", symbol->Name, static_cast<unsigned long long>(displacement));
        text += entry;
        ++found;
    }
    return text;
}

void log_game_threads(const char* reason) {
    std::vector<XboxThreadContext*> threads;
    {
        std::lock_guard<std::mutex> lock(g_threads_mutex);
        threads = g_threads;
    }
    log_line("%s: %zu Xbox threads", reason, threads.size());
    for (XboxThreadContext* context : threads) {
        if (context == current_xbox_thread()) continue;  // cannot suspend itself
        if (WaitForSingleObject(context->windows_thread, 0) == WAIT_OBJECT_0) continue;  // exited
        // Allocated before suspending: the thread may hold the heap's lock.
        std::string trace;
        trace.reserve(128);
        StackCopy copy;
        copy.bytes.reserve(64 * 1024);
        if (SuspendThread(context->windows_thread) == static_cast<DWORD>(-1)) continue;
        CONTEXT registers{};
        registers.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        const bool ok = GetThreadContext(context->windows_thread, &registers) != 0;
        if (ok) {
            const auto* stack = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(registers.Esp));
            MEMORY_BASIC_INFORMATION info{};
            VirtualQuery(stack, &info, sizeof(info));
            const auto* region_end =
                reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize);
            int found = 0;
            for (int i = 0; stack + i < region_end && i < 4096 && found < 10; ++i) {
                if (stack[i] >= g_code_begin && stack[i] < g_code_end) {
                    char entry[16];
                    std::snprintf(entry, sizeof(entry), " %08X", stack[i]);
                    trace += entry;
                    ++found;
                }
            }
            const size_t bytes = std::min<size_t>(64 * 1024, reinterpret_cast<const uint8_t*>(region_end) -
                                                                 reinterpret_cast<const uint8_t*>(stack));
            copy.base = registers.Esp;
            copy.bytes.assign(reinterpret_cast<const uint8_t*>(stack), reinterpret_cast<const uint8_t*>(stack) + bytes);
        }
        const int priority = GetThreadPriority(context->windows_thread);
        ResumeThread(context->windows_thread);
        if (ok) {
            log_line("  thread %u: priority %d eip %08lX irql %u eax %08lX ebx %08lX ecx %08lX edx %08lX esi %08lX "
                     "edi %08lX ebp %08lX, stack:%s",
                     context->thread_id, priority, registers.Eip, context->kpcr[kKpcrIrql], registers.Eax,
                     registers.Ebx, registers.Ecx, registers.Edx, registers.Esi, registers.Edi, registers.Ebp,
                     trace.c_str());
            log_line("    frames:%s", describe_frames(registers, copy).c_str());
        }
    }
}

void start_thread_watchdog(uint32_t code_begin, uint32_t code_end) {
    g_code_begin = code_begin;
    g_code_end = code_end;
    wchar_t value[16] = {};
    if (!GetEnvironmentVariableW(L"MC3_WATCHDOG", value, 16)) return;
    const DWORD period = static_cast<DWORD>(_wtoi(value)) * 1000;
    if (!period) return;
    std::thread([=] {
        for (;;) {
            Sleep(period);
            log_game_threads("watchdog");
        }
    }).detach();
}

}  // namespace mc3::pc
