#include "pc/kernel.h"

#include <windows.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <intrin.h>

#include "core/byteorder.h"
#include "core/kernel_exports.h"
#include "pc/kernel_internal.h"
#include "pc/log.h"

namespace mc3::pc {
namespace {

constexpr uint32_t kImportByOrdinal = 0x80000000;

std::array<const void*, kKernelOrdinalLimit> g_functions{};

// ---- Data exports --------------------------------------------------------------------------------------------

#pragma pack(push, 1)
struct AnsiString {
    uint16_t length;
    uint16_t maximum_length;
    uint32_t buffer;
};
struct XboxKrnlVersion {
    uint16_t major, minor, build, qfe;
};
struct XboxHardwareInfo {
    uint32_t flags;
    uint8_t gpu_revision;
    uint8_t mcp_revision;
    uint8_t reserved[2];
};
#pragma pack(pop)

char g_image_file_name_text[] = "\\Device\\CdRom0\\default.xbe";
char g_disk_model_text[] = "MC3PORT VIRTUAL DISK";
char g_disk_serial_text[] = "0000000000";

AnsiString make_ansi_string(char* text) {
    const auto length = static_cast<uint16_t>(std::strlen(text));
    return AnsiString{length, static_cast<uint16_t>(length + 1), address_of(text)};
}

XboxKrnlVersion g_krnl_version{1, 0, 5838, 1};  // final retail kernel
XboxHardwareInfo g_hardware_info{};             // TODO(phase 2): values the game checks, from Ghidra
uint32_t g_launch_data_page = 0;                // PLAUNCH_DATA_PAGE: no launch data, booted from the dashboard
AnsiString g_image_file_name = make_ansi_string(g_image_file_name_text);
uint32_t g_time_increment = 10000;  // 1 ms in 100 ns units
uint8_t g_debugger_enabled = 0;
uint8_t g_debugger_not_present = 1;
uint32_t g_disk_cache_partition_count = 3;
// TODO(phase 2): confirm in Ghidra whether these two are the string itself or a pointer to it.
AnsiString g_disk_model_number = make_ansi_string(g_disk_model_text);
AnsiString g_disk_serial_number = make_ansi_string(g_disk_serial_text);

// Opaque objects (object types, keys, handle table...): the game only compares addresses or reads zeros.
alignas(16) uint8_t g_zeroed_data[kKernelOrdinalLimit][0x100];

const void* data_export(uint32_t ordinal) {
    switch (ordinal) {
        case 40: return &g_disk_cache_partition_count;
        case 41: return &g_disk_model_number;
        case 42: return &g_disk_serial_number;
        case 88: return &g_debugger_enabled;
        case 89: return &g_debugger_not_present;
        case 120: return kernel_interrupt_time();
        case 154: return kernel_system_time();
        case 156: return kernel_tick_count();
        case 157: return &g_time_increment;
        case 164: return &g_launch_data_page;
        case 322: return &g_hardware_info;
        case 324: return &g_krnl_version;
        case 326: return &g_image_file_name;
        default: return g_zeroed_data[ordinal];
    }
}

// ---- Unimplemented functions ---------------------------------------------------------------------------------

// Reached from a generated stub: push ordinal ; call report_unimplemented. Stack on entry:
// [stub return] [ordinal] [game return address] [arg0] [arg1] ...
extern "C" [[noreturn]] void __cdecl report_unimplemented(uint32_t ordinal, uint32_t return_address, uint32_t arg0,
                                                          uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    const KernelExport* entry = find_kernel_export(ordinal);
    log_line("");
    log_line("kernel call not implemented: #%u %s", ordinal, entry ? entry->name : "?");
    log_line("  called from 0x%08X, first stack arguments: %08X %08X %08X %08X", return_address, arg0, arg1, arg2,
             arg3);
    TerminateProcess(GetCurrentProcess(), 3);
    for (;;) {
    }
}

// MC3_TRACE_KERNEL=<n>: log the first n kernel calls (name and caller). The wrapper saves every register, so
// it also works for the fastcall exports.
volatile long g_trace_remaining = 0;

extern "C" void __cdecl trace_kernel_call(uint32_t ordinal, uint32_t /*saved_registers*/[8], uint32_t return_address) {
    if (_InterlockedDecrement(&g_trace_remaining) < 0) return;
    const KernelExport* entry = find_kernel_export(ordinal);
    log_line("kernel: %s from %08X (thread %lu)", entry ? entry->name : "?", return_address, GetCurrentThreadId());
}

const void* make_trace_wrapper(ExecMemory& memory, uint32_t ordinal, const void* target) {
    uint8_t* code = memory.allocate(32);
    if (!code) return nullptr;
    uint8_t* p = code;
    *p++ = 0x60;  // pushad
    *p++ = 0xFF;  // push dword [esp + 32]: the caller's return address
    *p++ = 0x74;
    *p++ = 0x24;
    *p++ = 0x20;
    *p++ = 0x54;  // push esp
    *p++ = 0x68;  // push ordinal
    store_le32(p, ordinal);
    p += 4;
    emit_call(p, reinterpret_cast<const void*>(&trace_kernel_call));
    p += 5;
    *p++ = 0x83;  // add esp, 12
    *p++ = 0xC4;
    *p++ = 0x0C;
    *p++ = 0x61;  // popad
    *p = 0xE9;    // jmp target
    store_le32(p + 1, address_of(target) - (address_of(p) + 5));
    return code;
}

const void* make_unimplemented_stub(ExecMemory& memory, uint32_t ordinal) {
    uint8_t* code = memory.allocate(10);
    if (!code) return nullptr;
    code[0] = 0x68;  // push imm32
    store_le32(code + 1, ordinal);
    emit_call(code + 5, reinterpret_cast<const void*>(&report_unimplemented));
    return code;
}

}  // namespace

uint32_t kernel_launch_data_page() { return g_launch_data_page; }

void register_all_kernel_functions() {
    register_dispatcher_functions();
    register_thread_functions();
    register_memory_functions();
    register_system_functions();
    register_io_functions();
}

void register_kernel_function(uint32_t ordinal, const void* function) {
    if (ordinal < kKernelOrdinalLimit) g_functions[ordinal] = function;
}

bool install_kernel_thunks(const Xbe& xbe, ExecMemory& memory, KernelThunkReport* report, std::string* error) {
    char trace[16] = {};
    if (GetEnvironmentVariableA("MC3_TRACE_KERNEL", trace, sizeof(trace))) g_trace_remaining = std::atol(trace);
    auto* thunk = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(xbe.kernel_thunk_address()));
    for (; *thunk != 0; ++thunk) {
        const uint32_t import = *thunk;
        const uint32_t ordinal = import & ~kImportByOrdinal;
        const KernelExport* entry = find_kernel_export(ordinal);
        if (!(import & kImportByOrdinal) || !entry) {
            if (error) {
                char message[64];
                std::snprintf(message, sizeof(message), "unknown kernel import 0x%08X", import);
                *error = message;
            }
            return false;
        }

        const void* target = nullptr;
        if (entry->kind == KernelExportKind::Data) {
            target = data_export(ordinal);
            ++report->data;
        } else if (g_functions[ordinal]) {
            target = g_trace_remaining > 0 ? make_trace_wrapper(memory, ordinal, g_functions[ordinal])
                                           : g_functions[ordinal];
            ++report->implemented;
        } else {
            target = make_unimplemented_stub(memory, ordinal);
            ++report->unimplemented;
        }
        if (!target) {
            if (error) *error = "out of executable memory for kernel stubs";
            return false;
        }
        *thunk = address_of(target);
    }
    return true;
}

}  // namespace mc3::pc
