#include "pc/game_pause.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "pc/log.h"

namespace mc3::pc {
namespace {

std::mutex g_mutex;
std::condition_variable g_resumed;
std::atomic<bool> g_paused{false};
int64_t g_paused_total = 0;  // ticks of the pauses that ended
int64_t g_pause_start = 0;   // QueryPerformanceCounter when the current pause began

int64_t now_ticks() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

}  // namespace

void game_pause(bool paused) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (paused == g_paused) return;
        if (paused) {
            g_pause_start = now_ticks();
        } else {
            g_paused_total += now_ticks() - g_pause_start;
        }
        g_paused = paused;
    }
    if (!paused) g_resumed.notify_all();
    log_line("game %s", paused ? "paused" : "resumed");
}

bool game_paused() { return g_paused; }

void game_pause_point() {
    if (!g_paused) return;
    std::unique_lock<std::mutex> lock(g_mutex);
    g_resumed.wait(lock, [] { return !g_paused; });
}

int64_t game_paused_ticks() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_paused_total + (g_paused ? now_ticks() - g_pause_start : 0);
}

}  // namespace mc3::pc
