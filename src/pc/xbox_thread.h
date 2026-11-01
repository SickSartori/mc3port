#pragma once

#include <cstdint>
#include <functional>
#include <mutex>

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
    int dispatch_restore_priority = 0x7FFFFFFF;  // Windows priority to go back to below DISPATCH_LEVEL, or none
                                                  // (THREAD_PRIORITY_ERROR_RETURN)
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
// On the Xbox no thread runs while another is at DISPATCH_LEVEL. Here one at PASSIVE_LEVEL still can (when the
// owner blocks on a host lock, or Windows switches threads): the owner's thread id when that is the case, else 0.
uint32_t another_thread_at_dispatch();

// A host lock that game code takes (the kernel's dispatcher state, DirectSound's voices). On the Xbox such regions
// run at DISPATCH_LEVEL and nothing preempts them; here the interrupt or DPC thread can, and when it then needs the
// same lock it waits, and Windows runs other game threads on the game's core in the meantime. So a game thread
// holds the lock at the interrupt thread's priority: an interrupt that comes meanwhile waits until it lets go.
// Host threads (mixer, GPU) take it as a plain lock.
class GameLock {
public:
    explicit GameLock(std::mutex& mutex);
    ~GameLock();
    GameLock(const GameLock&) = delete;
    GameLock& operator=(const GameLock&) = delete;
    std::unique_lock<std::mutex>& get() { return lock_; }  // for condition variables

private:
    int restore_;  // the thread's priority before, or kNoRestore
    std::unique_lock<std::mutex> lock_;
};

// With MC3_WATCHDOG=<seconds>, periodically logs where every game thread is (EIP and return addresses into
// the game's code): the quickest way to see what a hung game is waiting for.
void start_thread_watchdog(uint32_t code_begin, uint32_t code_end);

// Logs every game thread once (what the watchdog prints), from any thread.
void log_game_threads(const char* reason);

// The Xbox has one CPU and game code may rely on it: every thread that runs game code (attached above) is kept on
// `mask`. The emulated hardware (GPU) is another processor and runs on the remaining cores, emulation_cores()
// (0 when there are none or no restriction is set).
void set_game_cores(unsigned long mask);
unsigned long emulation_cores();

// Shared by every thread: the fields of KPROCESS the game reads through KTHREAD.ApcState.Process.
uint8_t* xbox_process();

}  // namespace mc3::pc
