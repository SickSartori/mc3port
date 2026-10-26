#include "pc/nv2a.h"

#include <windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "pc/kernel_internal.h"
#include "pc/log.h"
#include "pc/mmio.h"
#include "pc/nv2a_kelvin.h"
#include "pc/renderer_d3d11.h"
#include "pc/physical_memory.h"
#include "pc/settings.h"

namespace mc3::pc {
namespace {

// --- register map (offsets from 0xFD000000) ---
constexpr uint32_t kPmcBoot0 = 0x000000;
constexpr uint32_t kPmcIntr0 = 0x000100;
constexpr uint32_t kPmcIntrEn0 = 0x000140;
constexpr uint32_t kPbusIntr0 = 0x001100;
constexpr uint32_t kPbusIntrEn0 = 0x001140;
constexpr uint32_t kPbusPciNv0 = 0x001800;  // PCI config space mirror: vendor/device
constexpr uint32_t kPbusPciNv2 = 0x001808;  // class code / revision
constexpr uint32_t kPfifoIntr0 = 0x002100;
constexpr uint32_t kPfifoIntrEn0 = 0x002140;
constexpr uint32_t kPfifoRamht = 0x002210;
constexpr uint32_t kPfifoRunoutStatus = 0x002400;
constexpr uint32_t kPfifoCaches = 0x002500;
constexpr uint32_t kPfifoCache1Status = 0x003214;
constexpr uint32_t kPfifoCache1DmaPush = 0x003220;
constexpr uint32_t kPfifoCache1DmaPut = 0x003240;
constexpr uint32_t kPfifoCache1DmaGet = 0x003244;
constexpr uint32_t kPfifoCache1Pull0 = 0x003250;
constexpr uint32_t kFifoEmpty = 1u << 4;
constexpr uint32_t kPtimerIntr0 = 0x009100;
constexpr uint32_t kPtimerIntrEn0 = 0x009140;
constexpr uint32_t kPtimerTime0 = 0x009400;
constexpr uint32_t kPtimerTime1 = 0x009410;
constexpr uint32_t kPfbCstatus = 0x10020C;  // RAM size
constexpr uint32_t kPfbWbc = 0x100410;      // write-back cache control
constexpr uint32_t kPfbWbcFlush = 1u << 16;
constexpr uint32_t kPgraphIntr = 0x400100;
constexpr uint32_t kPgraphIntrError = 1u << 20;  // software method trap (NO_OPERATION with an argument)
constexpr uint32_t kPgraphNsource = 0x400108;
constexpr uint32_t kPgraphNsourceNotification = 1;
constexpr uint32_t kPgraphIntrEn = 0x400140;
constexpr uint32_t kPgraphStatus = 0x400700;
constexpr uint32_t kPgraphTrappedAddr = 0x400704;  // method 0x1FFF, subchannel 0x70000, channel 0x1F00000
constexpr uint32_t kPgraphTrappedDataLow = 0x400708;
constexpr uint32_t kPgraphSurface = 0x400710;  // flip counters: WRITE_3D 0x700000, READ_3D 0x7000000, MODULO 0x70000000
constexpr uint32_t kPgraphIncrement = 0x40071C;
// Bit 0: PGRAPH takes methods from the FIFO. The driver's interrupt handler clears it while it reads a trapped
// method, so the GPU cannot overwrite TRAPPED_DATA with the next trap after the interrupt is acknowledged.
constexpr uint32_t kPgraphFifo = 0x400720;
std::atomic<bool> g_pgraph_fifo_enabled{true};
constexpr uint32_t kPgraphIncrementRead3d = 1u << 1;
// Written by the NV044 pattern object. D3D uses colour 0 as a progress marker: it puts a SET_MONOCHROME_COLOR0
// in the push buffer (push buffer address and a counter) and polls the register to know how far the GPU got.
constexpr uint32_t kPgraphPatternColor0 = 0x400B10;
constexpr uint32_t kPgraphPatternColor1 = 0x400B14;
constexpr uint32_t kPcrtcIntr0 = 0x600100;
constexpr uint32_t kPcrtcIntrEn0 = 0x600140;
constexpr uint32_t kPcrtcIntrVblank = 1;
constexpr uint32_t kPcrtcStart = 0x600800;
constexpr uint32_t kPcrtcRaster = 0x600808;
constexpr uint32_t kPraminBase = 0x700000;  // instance memory: real memory, never trapped
constexpr uint32_t kPraminSize = 0x100000;
constexpr uint32_t kUserDmaPut = 0x800040;  // channel 0 user area
constexpr uint32_t kUserDmaGet = 0x800044;
constexpr uint32_t kUserRef = 0x800048;

// PMC_INTR_0 summary bits.
constexpr uint32_t kPmcPfifo = 1u << 8;
constexpr uint32_t kPmcPgraph = 1u << 12;
constexpr uint32_t kPmcPtimer = 1u << 20;
constexpr uint32_t kPmcPcrtc = 1u << 24;
constexpr uint32_t kPmcPbus = 1u << 28;

constexpr uint32_t kGpuBusInterruptLevel = 3;
constexpr uint32_t kChipId = 0x02A000A2;  // NV2A revision A2, the retail chip

// --- Kelvin (NV097) methods handled so far ---
constexpr uint32_t kSetObject = 0x0000;
constexpr uint32_t kSetReference = 0x0050;
constexpr uint32_t kNoOperation = 0x0100;
constexpr uint32_t kSetFlipRead = 0x0120;
constexpr uint32_t kSetFlipWrite = 0x0124;
constexpr uint32_t kSetFlipModulo = 0x0128;
constexpr uint32_t kFlipIncrementWrite = 0x012C;
constexpr uint32_t kFlipStall = 0x0130;
constexpr uint32_t kSetContextDmaSemaphore = 0x01A4;
constexpr uint32_t kSetSemaphoreOffset = 0x1D6C;
constexpr uint32_t kBackEndWriteSemaphoreRelease = 0x1D70;

// Register file. Only the GPU emulation touches it directly; the game goes through the MMIO traps.
std::vector<uint32_t> g_registers(kNv2aRegisterSize / 4);
std::mutex g_register_mutex;
HANDLE g_put_written = nullptr;
HANDLE g_gpu_resume = nullptr;       // the CPU acknowledged a trap or advanced the flip read counter
HANDLE g_interrupt_request = nullptr;  // the GPU raised an interrupt; the interrupt thread runs the ISR
std::atomic<uint32_t> g_front_buffer{0};  // PCRTC_START: what the video output scans out

uint32_t& reg(uint32_t offset) { return g_registers[offset / 4]; }

// FIFO-level state; the 3D class itself lives in nv2a_kelvin.cpp.
struct Kelvin {
    uint32_t dma_semaphore = 0;  // base address of the semaphore DMA object
    uint32_t semaphore_offset = 0;
};
constexpr uint32_t kClassKelvin = 0x97;
uint8_t g_subchannel_class[8] = {};
bool g_renderer_ready = false;
std::atomic<bool> g_present_requested{false};

struct DisplayMode {
    uint32_t mode = 0;
    uint32_t format = 0;
    uint32_t pitch = 0;
    uint32_t frame_buffer = 0;
};

struct PushBufferState {
    std::atomic<uint32_t> get{0};
    uint32_t subroutine_return = 0;
    bool subroutine_active = false;
    uint32_t method = 0;
    uint32_t subchannel = 0;
    uint32_t method_count = 0;
    bool non_increasing = false;
};

Kelvin g_kelvin;
PushBufferState g_push;
std::mutex g_display_mutex;
DisplayMode g_display;
HWND g_window = nullptr;
std::atomic<uint64_t> g_frames{0};
std::map<uint32_t, uint32_t> g_unknown_methods;  // (subchannel << 16 | method) -> count
int g_trace_budget = 0;                          // MC3_TRACE_PB=<n>: log the first n methods
uint64_t g_trace_after_frame = 0;                // MC3_TRACE_PB_AFTER=<vblanks>: start tracing later
std::map<uint32_t, uint32_t> g_method_counts;    // (subchannel << 16 | method) -> count, MC3_GPU_STATS=1

// --- time ---

uint64_t now_nanoseconds() {
    static const LARGE_INTEGER frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<uint64_t>(counter.QuadPart / frequency.QuadPart * 1000000000ull +
                                 counter.QuadPart % frequency.QuadPart * 1000000000ull / frequency.QuadPart);
}

// --- register semantics ---

uint32_t pending_interrupts() {
    uint32_t pending = 0;
    if (reg(kPfifoIntr0) & reg(kPfifoIntrEn0)) pending |= kPmcPfifo;
    if (reg(kPgraphIntr) & reg(kPgraphIntrEn)) pending |= kPmcPgraph;
    if (reg(kPtimerIntr0) & reg(kPtimerIntrEn0)) pending |= kPmcPtimer;
    if (reg(kPcrtcIntr0) & reg(kPcrtcIntrEn0)) pending |= kPmcPcrtc;
    if (reg(kPbusIntr0) & reg(kPbusIntrEn0)) pending |= kPmcPbus;
    return pending;
}

uint32_t read_register(uint32_t offset) {
    std::lock_guard<std::mutex> lock(g_register_mutex);
    switch (offset) {
        case kPmcIntr0: return pending_interrupts();
        case kPtimerTime0: return static_cast<uint32_t>(now_nanoseconds()) & ~0x1Fu;
        case kPtimerTime1: return static_cast<uint32_t>(now_nanoseconds() >> 32) & 0x1FFFFFFF;
        case kPcrtcRaster: {
            constexpr uint64_t kFrameNs = 1000000000ull / 60;
            constexpr uint32_t kLines = 525;
            return static_cast<uint32_t>((now_nanoseconds() % kFrameNs) * kLines / kFrameNs);
        }
        case kUserDmaGet:
        case kPfifoCache1DmaGet: return g_push.get;
        case kPfifoCache1DmaPut: return reg(kUserDmaPut);  // the cache follows the channel's PUT
        // The emulated GPU executes commands as soon as PUT moves, so every FIFO and engine reports idle.
        case kPfifoRunoutStatus:
        case kPfifoCache1Status: return kFifoEmpty;
        case kPfifoCache1DmaPush: return reg(offset) & ~0x1010u;  // not busy, not suspended
        case kPfifoCaches: return reg(offset) & ~0x10u;           // no pull in progress
        case kPfifoCache1Pull0: return reg(offset) & ~0x1000u;    // puller idle
        case kPgraphStatus: return 0;
        default: return reg(offset);
    }
}

void write_register(uint32_t offset, uint32_t value) {
    std::lock_guard<std::mutex> lock(g_register_mutex);
    switch (offset) {
        case kPbusIntr0:
        case kPfifoIntr0:
        case kPtimerIntr0:
        case kPcrtcIntr0:
            reg(offset) &= ~value;  // write 1 to clear
            return;
        case kPgraphIntr:
            reg(offset) &= ~value;
            if (value & kPgraphIntrError) SetEvent(g_gpu_resume);  // the trapped software method is handled
            return;
        case kPgraphIncrement:
            if (value & kPgraphIncrementRead3d) {
                const uint32_t surface = reg(kPgraphSurface);
                const uint32_t modulo = std::max<uint32_t>(1, (surface >> 28) & 7);
                const uint32_t read = (((surface >> 24) & 7) + 1) % modulo;
                reg(kPgraphSurface) = (surface & ~0x07000000u) | (read << 24);
                SetEvent(g_gpu_resume);
            }
            return;
        case kPcrtcStart:
            reg(offset) = value;
            g_front_buffer = value;
            return;
        case kPmcIntr0:
            return;  // a summary of the other registers
        case kPgraphFifo:
            reg(offset) = value;
            g_pgraph_fifo_enabled = (value & 1) != 0;
            if (value & 1) SetEvent(g_gpu_resume);
            return;
        case kPgraphIntrEn:
        case kPmcIntrEn0:
            // Enabling may release an interrupt that was already pending (a trapped software method).
            reg(offset) = value;
            SetEvent(g_interrupt_request);
            return;
        case kPfbWbc:
            reg(offset) = value & ~kPfbWbcFlush;  // flushes complete at once: caches are coherent on the host
            return;
        case kUserDmaPut:
            reg(offset) = value;
            SetEvent(g_put_written);
            return;
        default:
            reg(offset) = value;
            return;
    }
}

uint32_t mmio_read(uint32_t address, uint32_t size) {
    const uint32_t offset = address - kNv2aRegisterBase;
    const uint32_t value = read_register(offset & ~3u);
    const uint32_t shift = (offset & 3) * 8;
    return size == 4 ? value : (value >> shift) & ((1u << (size * 8)) - 1);
}

void mmio_write(uint32_t address, uint32_t size, uint32_t value) {
    const uint32_t offset = address - kNv2aRegisterBase;
    if (size != 4) {
        const uint32_t shift = (offset & 3) * 8;
        const uint32_t mask = ((1u << (size * 8)) - 1) << shift;
        value = (read_register(offset & ~3u) & ~mask) | ((value << shift) & mask);
    }
    write_register(offset & ~3u, value);
}

const MmioDevice kDevice{mmio_read, mmio_write};

// --- objects ---

// Instance memory, as the driver wrote it through the PRAMIN window.
uint32_t ramin(uint32_t offset) {
    return *reinterpret_cast<const volatile uint32_t*>(static_cast<uintptr_t>(kNv2aRegisterBase + kPraminBase + offset));
}

struct GpuObject {
    uint32_t instance = 0;  // byte offset in instance memory
    uint32_t class_id = 0;
    uint32_t base = 0;   // DMA objects: physical start
    uint32_t limit = 0;  // DMA objects: last valid offset
};

std::map<uint32_t, GpuObject> g_objects;  // handle -> object, filled on first use

// Handles are looked up in the hash table (RAMHT) the driver keeps in instance memory. A linear scan is fine:
// lookups only happen when the driver binds an object, and results are cached.
const GpuObject* lookup_object(uint32_t handle) {
    const auto cached = g_objects.find(handle);
    if (cached != g_objects.end()) return &cached->second;

    uint32_t ramht_register = 0;
    {
        std::lock_guard<std::mutex> lock(g_register_mutex);
        ramht_register = reg(kPfifoRamht);
    }
    const uint32_t ramht_base = (ramht_register & 0x1F0) << 8;
    const uint32_t ramht_size = 0x1000u << ((ramht_register >> 16) & 3);
    for (uint32_t entry = ramht_base; entry < ramht_base + ramht_size && entry + 8 <= kPraminSize; entry += 8) {
        if (ramin(entry) != handle || !(ramin(entry + 4) & 0x80000000)) continue;
        GpuObject object;
        object.instance = (ramin(entry + 4) & 0xFFFF) << 4;
        const uint32_t flags = ramin(object.instance);
        object.class_id = flags & 0xFF;
        object.limit = ramin(object.instance + 4);
        object.base = (ramin(object.instance + 8) & 0xFFFFF000) | (flags >> 20);
        log_line("nv2a: object %08X: class %02X, instance %05X, base %08X, limit %08X", handle, object.class_id,
                 object.instance, object.base, object.limit);
        return &g_objects.emplace(handle, object).first->second;
    }
    log_line("nv2a: object handle %08X not found in RAMHT", handle);
    return nullptr;
}

uint32_t dma_base(uint32_t handle) {
    const GpuObject* object = lookup_object(handle);
    return object ? object->base : 0;
}

// --- methods ---

// --- statistics (MC3_GPU_STATS=1) ---

bool g_collect_statistics = false;

void log_method_statistics() {
    std::vector<std::pair<uint32_t, uint32_t>> sorted(g_method_counts.begin(), g_method_counts.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    log_line("nv2a: methods in the last 10 s (%zu distinct):", sorted.size());
    for (size_t i = 0; i < sorted.size() && i < 80; i += 4) {
        char line[256] = {};
        size_t used = 0;
        for (size_t j = i; j < i + 4 && j < sorted.size(); ++j) {
            used += std::snprintf(line + used, sizeof(line) - used, "  [%u] %04X x%-7u", sorted[j].first >> 16,
                                  sorted[j].first & 0xFFFF, sorted[j].second);
        }
        log_line("%s", line);
    }
    g_method_counts.clear();
}

// --- presentation ---

DisplayMode current_display() {
    std::lock_guard<std::mutex> lock(g_display_mutex);
    return g_display;
}

// On the GPU thread: shows the front buffer if the vertical blank asked for it.
void service_present() {
    if (!g_present_requested.exchange(false)) return;
    const DisplayMode display = current_display();
    const uint32_t front = g_front_buffer ? g_front_buffer.load() : display.frame_buffer;
    if (!front || !display.pitch) return;
    {  // DEBUG: game frames (changes of the shown buffer) per 10 s
        static uint32_t last_front = 0, flips = 0, presents = 0;
        if (front != last_front) ++flips, last_front = front;
        if (++presents == 600) {
            log_line("DEBUG display: %u flips in %u presents", flips, presents);
            flips = presents = 0;
        }
    }
    // The picture can be narrower than the pitch (widescreen: 720 of 768 pixels): what the game draws into.
    const uint32_t drawn = kelvin_drawn_width(front & 0x0FFFFFFF);
    const uint32_t width = drawn && drawn < display.pitch / 4 ? drawn : display.pitch / 4;
    renderer_present(front & 0x0FFFFFFF, width, 480, display.pitch);
}

// --- traps and flips: the GPU waits for the CPU ---

void set_flip_field_locked(uint32_t shift, uint32_t value) {
    reg(kPgraphSurface) = (reg(kPgraphSurface) & ~(7u << shift)) | ((value & 7) << shift);
}

void set_flip_field(uint32_t shift, uint32_t value) {
    std::lock_guard<std::mutex> lock(g_register_mutex);
    set_flip_field_locked(shift, value);
}

// While the driver keeps PGRAPH's FIFO access off (kPgraphFifo), no method runs.
void wait_for_pgraph_access() {
    while (!g_pgraph_fifo_enabled) {
        WaitForSingleObject(g_gpu_resume, 1);
        service_present();
    }
}

// The last software methods, for the hang detector: what the GPU asked the CPU and when it was answered.
struct TrapRecord {
    uint32_t method, argument, get;
    ULONGLONG raised_ms, acknowledged_ms;
};
TrapRecord g_traps[16] = {};
std::atomic<uint32_t> g_trap_count{0};

void log_recent_traps() {
    const uint32_t count = g_trap_count.load();
    const ULONGLONG now = GetTickCount64();
    log_line("nv2a: last software methods (of %u), PGRAPH_INTR %08X EN %08X, PMC_INTR_EN %08X, GET %08X PUT %08X",
             count, reg(kPgraphIntr), reg(kPgraphIntrEn), reg(kPmcIntrEn0), g_push.get.load(), reg(kUserDmaPut));
    for (uint32_t i = count > 16 ? count - 16 : 0; i < count; ++i) {
        const TrapRecord& r = g_traps[i % 16];
        log_line("  %04X(%08X) at GET %08X, %llu ms ago, answered after %lld ms", r.method, r.argument, r.get,
                 now - r.raised_ms, r.acknowledged_ms ? static_cast<long long>(r.acknowledged_ms - r.raised_ms) : -1ll);
    }
}

// NO_OPERATION with an argument is the driver's software method: the GPU stops and interrupts the CPU, whose
// handler reads the trapped method and data, does the work (callbacks, fences) and acknowledges by clearing the
// interrupt.
void trap_software_method(uint32_t subchannel, uint32_t method, uint32_t argument) {
    TrapRecord& record = g_traps[g_trap_count % 16];
    record = {method, argument, g_push.get.load(), GetTickCount64(), 0};
    ++g_trap_count;
    {
        // Like the hardware, the trap stays pending even while the interrupt is disabled (the driver's ISR may
        // have masked it for a moment): it is raised as soon as it is enabled again, and PGRAPH waits until then.
        std::lock_guard<std::mutex> lock(g_register_mutex);
        reg(kPgraphTrappedAddr) = (subchannel << 16) | method;
        reg(kPgraphTrappedDataLow) = argument;
        reg(kPgraphNsource) = kPgraphNsourceNotification;
        reg(kPgraphIntr) |= kPgraphIntrError;
    }
    SetEvent(g_interrupt_request);
    for (int waited = 0;; waited += 5) {
        {
            std::lock_guard<std::mutex> lock(g_register_mutex);
            if (!(reg(kPgraphIntr) & kPgraphIntrError)) {
                record.acknowledged_ms = GetTickCount64();
                return;
            }
        }
        WaitForSingleObject(g_gpu_resume, 5);
        service_present();
        if (waited == 2000) log_line("nv2a: software method %04X(%08X) not acknowledged after 2 s", method, argument);
    }
}

// FLIP_STALL: wait until the buffer about to be rendered into is no longer the one on screen, i.e. until the
// vertical blank ISR has advanced the read counter (NV_PGRAPH_INCREMENT) past it.
void flip_stall() {
    for (int waited = 0;; waited += 5) {
        {
            std::lock_guard<std::mutex> lock(g_register_mutex);
            const uint32_t surface = reg(kPgraphSurface);
            if (((surface >> 20) & 7) != ((surface >> 24) & 7)) return;
        }
        WaitForSingleObject(g_gpu_resume, 5);
        service_present();
        if (waited == 2000) log_line("nv2a: FLIP_STALL still waiting after 2 s");
    }
}

// --- 2D: NV062 (surface 2D) and NV09F (image blit) ---

constexpr uint32_t kClassPattern = 0x44;
constexpr uint32_t kClassSurface2d = 0x62;
constexpr uint32_t kClassImageBlit = 0x9F;

struct TwoD {
    uint32_t source_dma = 0, destination_dma = 0;  // base addresses
    uint32_t color_format = 0, pitch = 0, source_offset = 0, destination_offset = 0;
    uint32_t operation = 3, point_in = 0, point_out = 0;
} g_2d;

uint32_t bytes_per_pixel_2d(uint32_t color_format) {
    switch (color_format) {
        case 0x01: return 1;                        // Y8
        case 0x04: return 2;                        // R5G6B5
        case 0x06: case 0x07: case 0x0A: case 0x0B: return 4;  // X8R8G8B8, A8R8G8B8, Y32
        default: return 0;
    }
}

bool surface2d_method(uint32_t method, uint32_t argument) {
    switch (method) {
        case 0x0184: g_2d.source_dma = dma_base(argument); return true;
        case 0x0188: g_2d.destination_dma = dma_base(argument); return true;
        case 0x0300: g_2d.color_format = argument; return true;
        case 0x0304: g_2d.pitch = argument; return true;
        case 0x0308: g_2d.source_offset = argument; return true;
        case 0x030C: g_2d.destination_offset = argument; return true;
        default: return false;
    }
}

bool pattern_method(uint32_t method, uint32_t argument) {
    if (method != 0x0310 && method != 0x0314) return false;  // SET_MONOCHROME_COLOR0/1
    std::lock_guard<std::mutex> lock(g_register_mutex);
    reg(method == 0x0310 ? kPgraphPatternColor0 : kPgraphPatternColor1) = argument;
    return true;
}

bool image_blit_method(uint32_t method, uint32_t argument) {
    switch (method) {
        case 0x019C: return true;  // SET_CONTEXT_SURFACES: there is only one surface 2D object
        case 0x02FC: g_2d.operation = argument; return true;
        case 0x0300: g_2d.point_in = argument; return true;
        case 0x0304: g_2d.point_out = argument; return true;
        case 0x0308: {  // SIZE: does the copy
            const uint32_t bytes = bytes_per_pixel_2d(g_2d.color_format);
            if (g_2d.operation != 3 || !bytes) {
                log_line("nv2a: image blit operation %u, colour format %X not supported yet", g_2d.operation,
                         g_2d.color_format);
                return true;
            }
            BlitRequest blit{};
            blit.source = (g_2d.source_dma + g_2d.source_offset) & 0x0FFFFFFF;
            blit.destination = (g_2d.destination_dma + g_2d.destination_offset) & 0x0FFFFFFF;
            blit.source_pitch = g_2d.pitch & 0xFFFF;
            blit.destination_pitch = g_2d.pitch >> 16;
            blit.bytes_per_pixel = bytes;
            blit.source_x = g_2d.point_in & 0xFFFF;
            blit.source_y = g_2d.point_in >> 16;
            blit.destination_x = g_2d.point_out & 0xFFFF;
            blit.destination_y = g_2d.point_out >> 16;
            blit.width = argument & 0xFFFF;
            blit.height = argument >> 16;
            renderer_blit(blit);
            return true;
        }
        default: return false;
    }
}

void execute_method(uint32_t subchannel, uint32_t method, uint32_t argument) {
    wait_for_pgraph_access();
    if (g_collect_statistics) ++g_method_counts[(subchannel << 16) | method];
    if (g_trace_budget > 0 && g_frames >= g_trace_after_frame) {
        --g_trace_budget;
        log_line("nv2a: [%u] %04X = %08X", subchannel, method, argument);
    }
    switch (method) {
        case kSetObject: {
            const GpuObject* object = lookup_object(argument);
            g_subchannel_class[subchannel & 7] = object ? object->class_id : 0;
            return;
        }
        case kSetContextDmaSemaphore: g_kelvin.dma_semaphore = dma_base(argument); return;
        case kSetReference: {
            std::lock_guard<std::mutex> lock(g_register_mutex);
            reg(kUserRef) = argument;
            return;
        }
        case kNoOperation:
            if (argument) trap_software_method(subchannel, method, argument);
            return;
        case kSetFlipRead: set_flip_field(24, argument); return;
        case kSetFlipWrite: set_flip_field(20, argument); return;
        case kSetFlipModulo: set_flip_field(28, argument); return;
        case kFlipIncrementWrite: {
            std::lock_guard<std::mutex> lock(g_register_mutex);
            const uint32_t surface = reg(kPgraphSurface);
            const uint32_t modulo = std::max<uint32_t>(1, (surface >> 28) & 7);
            set_flip_field_locked(20, (((surface >> 20) & 7) + 1) % modulo);
            return;
        }
        case kFlipStall: flip_stall(); return;
        case kSetSemaphoreOffset: g_kelvin.semaphore_offset = argument; return;
        case kBackEndWriteSemaphoreRelease:
            if (uint8_t* semaphore = physical_pointer(g_kelvin.dma_semaphore + g_kelvin.semaphore_offset)) {
                *reinterpret_cast<volatile uint32_t*>(semaphore) = argument;
            }
            return;
        default: {
            const uint8_t class_id = g_subchannel_class[subchannel & 7];
            if (class_id == kClassKelvin && kelvin_method(method, argument)) return;
            if (class_id == kClassSurface2d && surface2d_method(method, argument)) return;
            if (class_id == kClassImageBlit && image_blit_method(method, argument)) return;
            if (class_id == kClassPattern && pattern_method(method, argument)) return;
            uint32_t& count = g_unknown_methods[(subchannel << 16) | method];
            if (count++ == 0) {
                log_line("nv2a: method %04X on subchannel %u (class %02X) not handled (first argument %08X)", method,
                         subchannel, class_id, argument);
            }
            return;
        }
    }
}

// Runs the push buffer from GET to PUT. The channel's push buffer DMA object starts at physical 0, so GET and
// PUT are physical addresses; the driver leaves a jump to the real buffer at address 0.
void run_push_buffer(uint32_t put) {
    PushBufferState& s = g_push;
    uint32_t get = s.get;
    while (get != put) {
        const uint8_t* pointer = physical_pointer(get);
        if (!pointer) {
            log_line("nv2a: push buffer GET %08X outside RAM, skipping to PUT %08X", get, put);
            get = put;
            break;
        }
        const uint32_t word = *reinterpret_cast<const uint32_t*>(pointer);
        get += 4;

        if (s.method_count) {
            execute_method(s.subchannel, s.method, word);
            if (!s.non_increasing) s.method += 4;
            --s.method_count;
            continue;
        }
        if (g_trace_budget > 0 && g_frames >= g_trace_after_frame && (word & 3)) {
            log_line("nv2a: %08X: control %08X", get - 4, word);
        }
        if ((word & 0xE0000003) == 0x20000000) {  // old-style jump
            get = word & 0x1FFFFFFF;
        } else if ((word & 3) == 1) {  // jump
            get = word & 0xFFFFFFFC;
        } else if ((word & 3) == 2) {  // call
            if (s.subroutine_active) log_line("nv2a: nested push buffer call at %08X", get - 4);
            s.subroutine_return = get;
            s.subroutine_active = true;
            get = word & 0xFFFFFFFC;
        } else if (word == 0x00020000) {  // return
            get = s.subroutine_return;
            s.subroutine_active = false;
        } else if ((word & 0xE0030003) == 0 || (word & 0xE0030003) == 0x40000000) {
            s.method = word & 0x1FFC;
            s.subchannel = (word >> 13) & 7;
            s.method_count = (word >> 18) & 0x7FF;
            s.non_increasing = (word & 0x40000000) != 0;
        } else {
            log_line("nv2a: unknown push buffer command %08X at %08X", word, get - 4);
        }
        s.get = get;
    }
    s.get = get;
}

void gpu_thread() {
    // The GPU is a processor of its own: off the game's core when there is another one. Above the game's threads
    // anyway: when it has to share the core, it must preempt a thread spinning on a register.
    if (const unsigned long cores = emulation_cores()) SetThreadAffinityMask(GetCurrentThread(), cores);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    g_renderer_ready = renderer_start(g_window);
    bool started = false;
    for (;;) {
        WaitForSingleObject(g_put_written, 5);
        service_present();
        uint32_t put = 0;
        {
            std::lock_guard<std::mutex> lock(g_register_mutex);
            put = reg(kUserDmaPut);
        }
        if (!started && put) {
            log_line("nv2a: first push buffer kick-off, PUT %08X", put);
            started = true;
        }
        if (started && put != g_push.get) run_push_buffer(put);
        static ULONGLONG last_statistics = GetTickCount64();
        if (g_collect_statistics && GetTickCount64() - last_statistics >= 10000) {
            last_statistics = GetTickCount64();
            log_method_statistics();
        }
    }
}

// --- display ---

// Full screen without a border on the window's monitor, and back to the window as it was (Alt+Enter or F11).
bool g_full_screen = false;

void toggle_full_screen(HWND window) {
    bool& full_screen = g_full_screen;
    static WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    if (!full_screen) {
        MONITORINFO monitor{sizeof(MONITORINFO)};
        if (!GetWindowPlacement(window, &placement) ||
            !GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitor)) {
            return;
        }
        SetWindowLongW(window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        const RECT& area = monitor.rcMonitor;
        SetWindowPos(window, HWND_TOP, area.left, area.top, area.right - area.left, area.bottom - area.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongW(window, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(window, &placement);
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    full_screen = !full_screen;
}

std::atomic<WindowHook> g_window_hook{nullptr};  // nv2a_set_window_hook

LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (const WindowHook hook = g_window_hook.load()) {
        LRESULT result = 0;
        if (hook(window, message, wparam, lparam, &result)) return result;
    }
    if (message == WM_CLOSE) {
        log_line("window closed by the user");
        ExitProcess(0);
    }
    const bool alt_enter = message == WM_SYSKEYDOWN && wparam == VK_RETURN && (lparam & (1 << 29));
    if (alt_enter || (message == WM_KEYDOWN && wparam == VK_F11)) {
        if (!(lparam & (1 << 30))) toggle_full_screen(window);  // not on auto-repeat
        return 0;
    }
    if (message == WM_SYSCHAR && wparam == VK_RETURN) return 0;  // no beep for Alt+Enter
    if (message == WM_SYSKEYDOWN && wparam == VK_F10) {  // F10 is a system key
        if (!(lparam & (1 << 30))) renderer_toggle_effects();
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

HICON g_icon = nullptr, g_small_icon = nullptr;  // nv2a_load_icons

void window_thread(HANDLE ready) {
    // Real pixels on scaled displays (150% and so on): otherwise Windows renders the window small and enlarges it,
    // blurring the picture.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_procedure;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = g_icon;
    window_class.hIconSm = g_small_icon;
    window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    window_class.lpszClassName = L"mc3port";
    RegisterClassExW(&window_class);
    // Client area in the picture's shape (renderer_aspect), 85% of the screen's usable height, at most its width.
    RECT work{0, 0, 1280, 1024};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    LONG height = std::max<LONG>(480, (work.bottom - work.top) * 85 / 100);
    height = std::min<LONG>(height, static_cast<LONG>((work.right - work.left) * 95 / 100 / renderer_aspect()));
    RECT rect{0, 0, static_cast<LONG>(height * renderer_aspect()), height};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    const int width = rect.right - rect.left, height_with_frame = rect.bottom - rect.top;  // centred in the work area
    g_window = CreateWindowW(L"mc3port", L"Midnight Club 3: DUB Edition Remix", WS_OVERLAPPEDWINDOW,
                             work.left + std::max<LONG>(0, (work.right - work.left - width) / 2),
                             work.top + std::max<LONG>(0, (work.bottom - work.top - height_with_frame) / 2), width,
                             height_with_frame, nullptr, nullptr, window_class.hInstance, nullptr);
    // A dark title bar (Windows 10 and 11), black on Windows 11 like the bars around the picture; set before the
    // window shows, so that it never flashes light.
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(g_window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const COLORREF black = RGB(0, 0, 0);
    DwmSetWindowAttribute(g_window, DWMWA_CAPTION_COLOR, &black, sizeof(black));
    ShowWindow(g_window, SW_SHOW);
    if (setting("fullscreen") != 0) toggle_full_screen(g_window);
    SetEvent(ready);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void present_front_buffer() {
    DisplayMode display;
    {
        std::lock_guard<std::mutex> lock(g_display_mutex);
        display = g_display;
    }
    uint32_t start = 0;
    {
        std::lock_guard<std::mutex> lock(g_register_mutex);
        start = reg(kPcrtcStart);
    }
    if (!start) start = display.frame_buffer;
    if (!g_window || !display.pitch || !start) return;

    // D3DFMT_LIN_R5G6B5 (0x11) and X1R5G5B5 (0x10) are the 16-bit scan-out formats; the rest are 32-bit.
    const bool is_16bit = display.format == 0x11 || display.format == 0x10;
    const uint32_t bytes_per_pixel = is_16bit ? 2 : 4;
    const int width = static_cast<int>(display.pitch / bytes_per_pixel);
    const int height = 480;
    const uint8_t* pixels = physical_pointer(start);
    if (!pixels || !physical_pointer(start + display.pitch * height - 1)) return;

    struct {
        BITMAPINFOHEADER header;
        DWORD masks[3];
    } info{};
    info.header.biSize = sizeof(BITMAPINFOHEADER);
    info.header.biWidth = width;
    info.header.biHeight = -height;  // top-down
    info.header.biPlanes = 1;
    info.header.biBitCount = static_cast<WORD>(bytes_per_pixel * 8);
    info.header.biCompression = is_16bit ? BI_BITFIELDS : BI_RGB;
    info.masks[0] = 0xF800;
    info.masks[1] = 0x07E0;
    info.masks[2] = 0x001F;

    RECT client;
    GetClientRect(g_window, &client);
    HDC dc = GetDC(g_window);
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, 0, 0, client.right, client.bottom, 0, 0, width, height, pixels,
                  reinterpret_cast<const BITMAPINFO*>(&info), DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(g_window, dc);
}

// Runs the GPU's interrupt service routine for the vertical blank (60 Hz) and whenever the GPU raises an
// interrupt (software method traps). One thread delivers all of them, like the single interrupt line.
void interrupt_thread() {
    // The interrupt service routine is game code: it needs an Xbox thread context like any game thread.
    constexpr uint32_t kTlsAreaSize = 0x100;
    attach_xbox_thread_context(XbeTlsTemplate{}, kTlsAreaSize);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    static const bool interrupts_disabled = GetEnvironmentVariableW(L"MC3_NO_GPU_IRQ", nullptr, 0) != 0;

    using Clock = std::chrono::steady_clock;
    // The game shows a frame every second vertical blank: 30 fps at the Xbox's 60 Hz. Its logic runs on real time
    // (the same acceleration at 60 and 120 Hz), so 120 Hz blanks give 60 game frames per second (setting frame_rate).
    static const int vblank_hz = setting("frame_rate") >= 60 ? 120 : 60;
    const auto period = std::chrono::nanoseconds(1000000000 / vblank_hz);
    auto next = Clock::now() + period;
    for (;;) {
        const auto now = Clock::now();
        const DWORD wait = now >= next ? 0 : static_cast<DWORD>(
            std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count() + 1);
        WaitForSingleObject(g_interrupt_request, wait);
        const bool vblank = Clock::now() >= next;

        bool raise = false;
        {
            std::lock_guard<std::mutex> lock(g_register_mutex);
            if (vblank) reg(kPcrtcIntr0) |= kPcrtcIntrVblank;
            raise = pending_interrupts() && reg(kPmcIntrEn0);
        }
        if (raise && !interrupts_disabled) trigger_interrupt(kGpuBusInterruptLevel);
        if (!vblank) continue;
        next += period;
        if (Clock::now() > next + period * 4) next = Clock::now() + period;  // do not try to catch up

        if (g_renderer_ready) {
            g_present_requested = true;
            SetEvent(g_put_written);  // wake the GPU thread to present
        } else {
            present_front_buffer();
        }
        if (++g_frames % 600 == 0) {
            log_line("nv2a: %llu vblanks, push buffer GET %08X, front buffer %08X",
                     static_cast<unsigned long long>(g_frames.load()), g_push.get.load(), reg(kPcrtcStart));
        }
        // Hang detector without an extra thread (which changes the scheduling): the push buffer has not moved for
        // 10 s, so log once where the game threads are.
        static uint32_t last_get = 0;
        static uint32_t still_frames = 0;
        const uint32_t get = g_push.get.load();
        still_frames = (get == last_get) ? still_frames + 1 : 0;
        last_get = get;
        if (still_frames == 600 || still_frames == 660 || still_frames == 720) {
            log_game_threads("nv2a: push buffer idle for 10 s");  // three samples, to tell a loop from a wait
            if (still_frames == 600) log_recent_traps();
        }
    }
}

}  // namespace

bool start_nv2a() {
    // Registers: reserved but not committed, so every access traps into nv2a_handle_access_violation.
    // Instance memory (PRAMIN) is ordinary memory in the middle of that range.
    auto* base = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(static_cast<uintptr_t>(kNv2aRegisterBase)),
                                                    kNv2aRegisterSize, MEM_RESERVE, PAGE_NOACCESS));
    if (!base || !VirtualAlloc(base + kPraminBase, kPraminSize, MEM_COMMIT, PAGE_READWRITE)) {
        log_line("cannot map the NV2A registers at %08X (error %lu)", kNv2aRegisterBase, GetLastError());
        return false;
    }
    wchar_t trace[16] = {};
    if (GetEnvironmentVariableW(L"MC3_TRACE_PB", trace, 16)) g_trace_budget = _wtoi(trace);
    if (GetEnvironmentVariableW(L"MC3_TRACE_PB_AFTER", trace, 16)) g_trace_after_frame = _wtoi(trace);
    g_collect_statistics = GetEnvironmentVariableW(L"MC3_GPU_STATS", nullptr, 0) != 0;
    g_put_written = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_gpu_resume = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_interrupt_request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    reg(kPmcBoot0) = kChipId;
    reg(kPbusPciNv0) = 0x02A010DE;  // NVIDIA, NV2A
    reg(kPbusPciNv2) = 0x030000A2;  // VGA controller, revision A2
    reg(kPfbCstatus) = kPhysicalMemorySize;
    reg(kPgraphFifo) = 1;  // PGRAPH takes methods

    nv2a_open_window();
    kelvin_initialise(&dma_base);
    std::thread(gpu_thread).detach();
    start_game_code_thread(interrupt_thread);
    log_line("nv2a: registers at %08X, window %p", kNv2aRegisterBase, static_cast<void*>(g_window));
    return true;
}

bool nv2a_handle_access_violation(CONTEXT* context, uint32_t address) {
    const uint32_t offset = address - kNv2aRegisterBase;
    if (offset >= kNv2aRegisterSize || (offset >= kPraminBase && offset < kPraminBase + kPraminSize)) return false;
    if (emulate_mmio_access(context, address, kDevice)) return true;
    const auto* code = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(context->Eip));
    log_line("nv2a: cannot emulate the register access at %08lX (address %08X, code %02X %02X %02X %02X)",
             context->Eip, address, code[0], code[1], code[2], code[3]);
    return false;
}

HWND nv2a_open_window() {
    if (!g_window) {
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::thread(window_thread, ready).detach();
        WaitForSingleObject(ready, 5000);
        CloseHandle(ready);
    }
    return g_window;
}

void nv2a_set_window_hook(WindowHook hook) { g_window_hook = hook; }

void nv2a_set_full_screen(bool full_screen) {
    if (g_window && full_screen != g_full_screen) toggle_full_screen(g_window);
}

void nv2a_load_icons() {
    // Each size picked from the .ico (res/mc3.rc, resource 1), not shrunk from a large one.
    HINSTANCE module = GetModuleHandleW(nullptr);
    const UINT dpi = GetDpiForSystem();
    g_icon = static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetricsForDpi(SM_CXICON, dpi),
                                           GetSystemMetricsForDpi(SM_CYICON, dpi), 0));
    g_small_icon = static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                 GetSystemMetricsForDpi(SM_CXSMICON, dpi),
                                                 GetSystemMetricsForDpi(SM_CYSMICON, dpi), 0));
    if (!g_icon) log_line("window: the icon could not be loaded (error %lu)", GetLastError());
}

void nv2a_set_display_mode(uint32_t mode, uint32_t format, uint32_t pitch, uint32_t frame_buffer) {
    log_line("AvSetDisplayMode: mode %08X format %08X pitch %u frame buffer %08X", mode, format, pitch, frame_buffer);
    std::lock_guard<std::mutex> lock(g_display_mutex);
    g_display = DisplayMode{mode, format, pitch, frame_buffer};
}

}  // namespace mc3::pc
