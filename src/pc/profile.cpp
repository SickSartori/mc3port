#include "pc/profile.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pc/log.h"
#include "pc/nv2a.h"

namespace mc3::pc {
namespace {

constexpr ULONGLONG kIntervalMilliseconds = 10000;
constexpr size_t kKinds = static_cast<size_t>(ProfileTime::count);
const char* const kKindNames[kKinds] = {"object waits", "sleeps",     "dispatch lock", "idle",
                                        "present",      "trap waits", "flip waits",    "access waits"};

struct ThreadProfile {
    char name[32] = {};
    bool game_core = false;
    HANDLE handle = nullptr;
    std::atomic<uint64_t> ticks[kKinds] = {};
    uint64_t last_cpu = 0, last_kernel = 0;  // 100 ns units, at the last log
};

thread_local ThreadProfile* t_profile = nullptr;
std::mutex g_threads_mutex;
std::vector<ThreadProfile*> g_threads;  // never freed: a thread that exits stays at zero

const bool g_detailed = GetEnvironmentVariableW(L"MC3_PROFILE", nullptr, 0) != 0;

// GPU thread only.
uint32_t g_frames = 0;
uint32_t g_blanks[5] = {};  // frames that took 1, 2, 3, 4, 5 or more blanks

std::atomic<uint32_t> g_register_accesses{0}, g_privileged{0};
std::mutex g_registers_mutex;
std::unordered_map<uint64_t, uint32_t> g_registers;  // offset << 32 | game code address -> accesses (MC3_PROFILE=1)

double ticks_per_millisecond() {
    static const double value = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    return value;
}

uint64_t as_uint64(const FILETIME& time) {
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

// CPU time of the thread so far (kernel + user) and its kernel part, in 100 ns units.
std::pair<uint64_t, uint64_t> cpu_time(HANDLE thread) {
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(thread, &created, &exited, &kernel, &user)) return {0, 0};
    return {as_uint64(kernel) + as_uint64(user), as_uint64(kernel)};
}

}  // namespace

void profile_name_thread(bool game_core, const char* format, ...) {
    char name[32];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(name, sizeof(name), format, arguments);
    va_end(arguments);
    std::lock_guard<std::mutex> lock(g_threads_mutex);
    if (!t_profile) {
        auto* profile = new ThreadProfile{};
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &profile->handle, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        std::tie(profile->last_cpu, profile->last_kernel) = cpu_time(profile->handle);
        g_threads.push_back(profile);
        t_profile = profile;
    }
    std::snprintf(t_profile->name, sizeof(t_profile->name), "%s", name);
    t_profile->game_core = game_core;
}

uint64_t profile_ticks() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(now.QuadPart);
}

void profile_add(ProfileTime kind, uint64_t ticks) {
    if (t_profile) t_profile->ticks[static_cast<size_t>(kind)].fetch_add(ticks, std::memory_order_relaxed);
}

void profile_frame_shown(uint32_t vblanks) {
    ++g_frames;
    ++g_blanks[std::clamp<uint32_t>(vblanks, 1, 5) - 1];
}

void profile_register_access(uint32_t offset, uint32_t eip) {
    g_register_accesses.fetch_add(1, std::memory_order_relaxed);
    if (!g_detailed) return;
    std::lock_guard<std::mutex> lock(g_registers_mutex);
    ++g_registers[static_cast<uint64_t>(offset) << 32 | eip];
}

void profile_privileged_instruction() { g_privileged.fetch_add(1, std::memory_order_relaxed); }

void profile_log_if_due() {
    static ULONGLONG since = 0;
    const ULONGLONG now = GetTickCount64();
    if (!since) since = now;
    if (now - since < kIntervalMilliseconds) return;
    const double seconds = (now - since) / 1000.0;
    since = now;

    // Per second of real time: milliseconds a thread ran, or spent on each kind of wait.
    struct Row {
        std::string name;
        bool game_core;
        double cpu, kernel, kinds[kKinds];
    };
    std::vector<Row> rows;
    {
        std::lock_guard<std::mutex> lock(g_threads_mutex);
        for (ThreadProfile* profile : g_threads) {
            const auto [cpu, kernel] = cpu_time(profile->handle);
            Row row{profile->name, profile->game_core, (cpu - profile->last_cpu) / 10000.0 / seconds,
                    (kernel - profile->last_kernel) / 10000.0 / seconds, {}};
            profile->last_cpu = cpu;
            profile->last_kernel = kernel;
            for (size_t i = 0; i < kKinds; ++i) {
                row.kinds[i] = profile->ticks[i].exchange(0) / ticks_per_millisecond() / seconds;
            }
            rows.push_back(row);
        }
    }
    double game_core = 0, gpu = 0;
    for (const Row& row : rows) {
        if (row.game_core) game_core += row.cpu;
        if (row.name == "gpu") gpu = row.cpu;
    }
    const uint32_t frames = std::max<uint32_t>(g_frames, 1);
    log_line("profile: %.1f game fps, blanks per frame at %d Hz: 1 %u%%, 2 %u%%, 3 %u%%, 4 %u%%, 5+ %u%%; "
             "game core %.0f%% busy, GPU thread %.0f%% busy",
             g_frames / seconds, nv2a_vblank_hz(), g_blanks[0] * 100 / frames, g_blanks[1] * 100 / frames,
             g_blanks[2] * 100 / frames, g_blanks[3] * 100 / frames, g_blanks[4] * 100 / frames, game_core / 10.0,
             gpu / 10.0);
    g_frames = 0;
    std::fill(std::begin(g_blanks), std::end(g_blanks), 0u);
    const uint32_t accesses = g_register_accesses.exchange(0), privileged = g_privileged.exchange(0);
    if (!g_detailed) return;

    // Per thread: CPU, then the timed waits; "other" is the rest of the second (running, or ready on a busy core).
    for (const Row& row : rows) {
        double timed = 0;
        for (const double kind : row.kinds) timed += kind;
        if (row.cpu < 1 && timed < 1) continue;
        char line[384];
        size_t used = std::snprintf(line, sizeof(line), "profile:   %-16s%s CPU %4.0f ms/s (kernel %3.0f)",
                                    row.name.c_str(), row.game_core ? " [game core]" : "", row.cpu, row.kernel);
        for (size_t i = 0; i < kKinds && used < sizeof(line); ++i) {
            if (row.kinds[i] >= 0.5) {
                used += std::snprintf(line + used, sizeof(line) - used, ", %s %.0f", kKindNames[i], row.kinds[i]);
            }
        }
        if (used < sizeof(line)) {
            std::snprintf(line + used, sizeof(line) - used, ", other %.0f", std::max(0.0, 1000.0 - timed));
        }
        log_line("%s", line);
    }

    std::vector<std::pair<uint64_t, uint32_t>> registers;
    {
        std::lock_guard<std::mutex> lock(g_registers_mutex);
        registers.assign(g_registers.begin(), g_registers.end());
        g_registers.clear();
    }
    std::sort(registers.begin(), registers.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    char line[384];
    size_t used = std::snprintf(line, sizeof(line), "profile:   GPU register accesses %.0f/s, privileged instructions "
                                                    "%.0f/s; most used (register at code):",
                                accesses / seconds, privileged / seconds);
    for (size_t i = 0; i < registers.size() && i < 8 && used < sizeof(line); ++i) {
        used += std::snprintf(line + used, sizeof(line) - used, " %06X@%08X %.0f/s",
                              static_cast<uint32_t>(registers[i].first >> 32),
                              static_cast<uint32_t>(registers[i].first), registers[i].second / seconds);
    }
    log_line("%s", line);
}

}  // namespace mc3::pc
