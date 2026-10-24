#include "pc/xbox_thread.h"

#include <windows.h>
#include <intrin.h>
#include <malloc.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <mutex>

#include "core/byteorder.h"
#include "pc/exec_memory.h"
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
std::recursive_mutex g_dispatch_level_lock;

alignas(16) uint8_t g_process[kKProcessSize] = {};
std::once_flag g_process_once;

}  // namespace

XboxThreadContext* attach_xbox_thread_context(const XbeTlsTemplate& tls, uint32_t tls_area_size) {
    std::call_once(g_process_once, [] {
        store_le32(g_process + kKProcessThreadQuantum, 60);  // 20 ms, the XAPI default
        g_process[kKProcessBasePriority] = kNormalBasePriority;
    });

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

uint8_t raise_irql(uint8_t new_irql) {
    const uint8_t old_irql = current_irql();
    if (old_irql < kDispatchLevel && new_irql >= kDispatchLevel) g_dispatch_level_lock.lock();
    if (t_context) t_context->kpcr[kKpcrIrql] = new_irql;
    return old_irql;
}

void lower_irql(uint8_t new_irql) {
    const uint8_t old_irql = current_irql();
    if (t_context) t_context->kpcr[kKpcrIrql] = new_irql;
    if (old_irql >= kDispatchLevel && new_irql < kDispatchLevel) g_dispatch_level_lock.unlock();
}

uint8_t* xbox_process() { return g_process; }

}  // namespace mc3::pc
