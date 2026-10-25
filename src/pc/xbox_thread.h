#pragma once

#include <cstdint>
#include <functional>

namespace mc3::pc {

// On Xbox, fs: points at the KPCR; on Windows it points at the TEB. Game reads of fs:[offset] are patched to
// read from an emulated KPCR instead, found through this TEB field (NtTib.ArbitraryUserPointer), which the
// game never touches.
constexpr uint32_t kXboxContextFsSlot = 0x14;

// Offsets above this are not part of the KPCR the game can see.
constexpr uint32_t kXboxKpcrSize = 0x300;
constexpr uint32_t kXboxKThreadSize = 0x400;  // fields up to +0x12C are read; leave room for more

constexpr uint8_t kPassiveLevel = 0;
constexpr uint8_t kDispatchLevel = 2;

// Initial TLS contents taken from the mapped XBE.
struct XbeTlsTemplate {
    const uint8_t* data = nullptr;
    uint32_t data_size = 0;
    uint32_t zero_fill_size = 0;
};

// Everything the emulated kernel keeps per game thread. `kthread` is what the game knows as the thread object
// (ETHREAD): it starts with a dispatcher header, so it can be waited on.
struct XboxThreadContext {
    alignas(16) uint8_t kpcr[kXboxKpcrSize];
    alignas(16) uint8_t kthread[kXboxKThreadSize];
    uint8_t* tls_area;  // [0] = pointer to the TLS data that follows; ends at the emulated StackBase
    void* windows_thread;
    uint32_t thread_id;
    int32_t base_priority_increment;
};

// Builds the emulated KPCR, KTHREAD and TLS area for the calling thread and binds them to it.
// Must run on the thread that will execute game code, before any of it runs. `tls_area_size` is the size
// the game asked for (PsCreateSystemThreadEx) or 0 to derive it from the template.
XboxThreadContext* attach_xbox_thread_context(const XbeTlsTemplate& tls, uint32_t tls_area_size);

// nullptr on threads that never run game code.
XboxThreadContext* current_xbox_thread();
XboxThreadContext* xbox_thread_from_kthread(uint32_t kthread_address);
uint32_t kthread_address(const XboxThreadContext* context);

// Host thread that will run game code (ISRs, DPCs): its whole stack is committed up front, because the Xbox
// CRT does not probe large stack frames page by page and would skip Windows' guard page.
void start_game_code_thread(std::function<void()> body);

// IRQL of the calling thread. Raising to DISPATCH_LEVEL or above takes a process-wide lock: on the
// single-core Xbox nothing else runs at that point, and game code relies on it.
uint8_t raise_irql(uint8_t new_irql);
void lower_irql(uint8_t new_irql);
uint8_t current_irql();

// With MC3_WATCHDOG=<seconds>, periodically logs where every game thread is (EIP and return addresses into
// the game's code): the quickest way to see what a hung game is waiting for.
void start_thread_watchdog(uint32_t code_begin, uint32_t code_end);

// Logs every game thread once (what the watchdog prints), from any thread.
void log_game_threads(const char* reason);

// Shared by every thread: the fields of KPROCESS the game reads through KTHREAD.ApcState.Process.
uint8_t* xbox_process();

}  // namespace mc3::pc
