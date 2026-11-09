#pragma once

#include <cstdint>

namespace mc3::pc {

// Where the time goes, logged every 10 s from the GPU thread. Always: the game's frames per second, how many
// vertical blanks each one took, and how busy the game's core and the GPU thread were. With MC3_PROFILE=1 also
// every named thread's CPU time and what it waited for, the GPU registers the game reached through exceptions, and a
// line for each frame that missed its time, with where the time since the frame before went.

// Time a thread spends outside its own work, measured by the thread itself.
enum class ProfileTime : uint8_t {
    object_waits,   // kernel waits on dispatcher objects (events, semaphores, threads)
    sleeps,         // KeDelayExecutionThread, NtYieldExecution
    dispatch_lock,  // raising to DISPATCH_LEVEL while another thread is there
    idle,           // host loops waiting for work (GPU thread, interrupt and DPC threads)
    present,        // GPU thread: renderer_present (effects, the window, Present)
    trap_waits,     // GPU thread: a software method waits for the CPU's interrupt handler
    flip_waits,     // GPU thread: FLIP_STALL waits for the vertical blank
    access_waits,   // GPU thread: PGRAPH's FIFO access is off
    shader_compile, // D3DCompile, on the thread that needs the shader
    texture_upload, // decoding a texture and creating it
    file_reads,     // game threads: blocked reading the disc image
    count
};

// The calling thread appears in the table under this name (printf format); a second call renames it. Threads on
// the game's core (every thread that runs game code) add up to its load.
void profile_name_thread(bool game_core, const char* format, ...);
uint64_t profile_ticks();
void profile_add(ProfileTime kind, uint64_t ticks);  // for the calling thread
void profile_frame_shown(uint32_t vblanks);          // GPU thread
void profile_register_access(uint32_t offset, uint32_t eip);
void profile_privileged_instruction();
bool profile_detailed();  // MC3_PROFILE=1
// A game thread at PASSIVE_LEVEL reached `site` (called from game code at `caller`) while thread `owner` was at
// DISPATCH_LEVEL: something the single-processor Xbox never allows. Always counted; the first few are logged.
void profile_exclusion_break(const char* site, uint32_t caller, uint32_t owner);
// A thread at DISPATCH_LEVEL found a GameLock busy and spun `ticks` for it; `waited` if it then waited after all.
void profile_dispatch_spin(uint64_t ticks, bool waited);
void profile_log_if_due();  // GPU thread

class ProfileScope {
public:
    explicit ProfileScope(ProfileTime kind) : kind_(kind), start_(profile_ticks()) {}
    ~ProfileScope() { profile_add(kind_, profile_ticks() - start_); }
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

private:
    ProfileTime kind_;
    uint64_t start_;
};

}  // namespace mc3::pc
