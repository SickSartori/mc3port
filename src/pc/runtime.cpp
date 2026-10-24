// mc3runtime.dll: loaded by the launcher (mc3.exe), takes over the launcher's address range and runs the game.
//
// Order matters: everything that needs the launcher's PE header or the Windows loader's view of mc3.exe
// (module path, image size, command line) happens before map_xbe() overwrites the image with the XBE.

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "core/xbe.h"
#include "core/xdvdfs.h"
#include "pc/exec_memory.h"
#include "pc/dsound_hle.h"
#include "pc/fs_patch.h"
#include "pc/hle_patches.h"
#include "pc/kernel.h"
#include "pc/kernel_internal.h"
#include "pc/log.h"
#include "pc/nv2a.h"
#include "pc/physical_memory.h"
#include "pc/privileged.h"
#include "pc/xbox_thread.h"

static_assert(sizeof(void*) == 4, "the PC runtime runs the game's x86 code natively and must be 32-bit");

namespace mc3::pc {
namespace {

constexpr uint32_t kMainThreadStackSize = 1024 * 1024;

struct HostImage {
    uint8_t* base;
    uint32_t size;
};

struct MainThreadStart {
    uint32_t entry_point;
    XbeTlsTemplate tls;
};

std::unique_ptr<XdvdfsImage> g_disc;
std::optional<Xbe> g_xbe;
ExecMemory g_exec_memory;
MainThreadStart g_main_thread_start;

[[noreturn]] void fatal(const char* format, ...) {
    char message[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    log_line("FATAL: %s", message);
    // MC3_NO_DIALOG keeps automated runs from blocking on the message box.
    if (!GetEnvironmentVariableW(L"MC3_NO_DIALOG", nullptr, 0)) {
        MessageBoxA(nullptr, message, "mc3port", MB_ICONERROR | MB_OK);
    }
    ExitProcess(1);
}

HostImage host_image() {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    return HostImage{base, nt->OptionalHeader.SizeOfImage};
}

std::filesystem::path module_directory() {
    wchar_t path[MAX_PATH * 4];
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return std::filesystem::path(std::wstring(path, length)).parent_path();
}

// First command-line argument, else the first .iso next to mc3.exe or in the working directory.
std::filesystem::path find_disc_image(const std::filesystem::path& exe_directory) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::filesystem::path result;
    if (argv && argc > 1) result = argv[1];
    LocalFree(argv);
    if (!result.empty()) return result;

    std::error_code ignored;
    for (const std::filesystem::path& directory : {exe_directory, std::filesystem::current_path(ignored)}) {
        for (const auto& item : std::filesystem::directory_iterator(directory, ignored)) {
            std::wstring extension = item.path().extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            if (item.is_regular_file(ignored) && extension == L".iso") return item.path();
        }
    }
    return {};
}

void log_xbe(const Xbe& xbe) {
    log_line("XBE: '%s' title id %08X, %s", xbe.title().c_str(), xbe.title_id(), xbe.is_retail() ? "retail" : "debug");
    log_line("  base %08X, image %08X bytes, entry %08X, kernel thunk %08X", xbe.base_address(), xbe.image_size(),
             xbe.entry_point(), xbe.kernel_thunk_address());
    std::string libraries;
    for (const XbeLibrary& library : xbe.libraries()) {
        libraries += " " + library.name + "." + std::to_string(library.build);
    }
    log_line("  XDK libraries:%s", libraries.c_str());
}

// Windows keeps reading the PE header at the image base after startup: the kernel takes the default stack
// sizes of every new thread from it. The game needs its XBE header at the same address, so both live there.
// The XBE's first 0x104 bytes are its magic and RSA signature, unused once loaded: a minimal PE header
// (no sections, no data directories) fits in them. Only the magic changes, "XBEH" -> "MZEH"; the game's code
// never checks it (no code section contains the constant).
void write_hybrid_pe_header(uint8_t* base, const std::vector<uint8_t>& launcher_header) {
    constexpr uint32_t kNtHeadersOffset = 0x40;
    constexpr uint32_t kXbeFirstUsedField = 0x104;  // XBE base address field
    constexpr uint32_t kOptionalHeaderSize = offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory);
    static_assert(kNtHeadersOffset + offsetof(IMAGE_NT_HEADERS32, OptionalHeader) + kOptionalHeaderSize <=
                  kXbeFirstUsedField);

    const auto* old_dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(launcher_header.data());
    const auto* old_nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(launcher_header.data() + old_dos->e_lfanew);

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = kNtHeadersOffset;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + kNtHeadersOffset);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader = old_nt->FileHeader;
    nt->FileHeader.NumberOfSections = 0;
    nt->FileHeader.SizeOfOptionalHeader = kOptionalHeaderSize;
    std::memcpy(&nt->OptionalHeader, &old_nt->OptionalHeader, kOptionalHeaderSize);
    nt->OptionalHeader.NumberOfRvaAndSizes = 0;
}

// Replaces the launcher image with the XBE: header at the base, preloaded sections at their addresses.
void map_xbe(const Xbe& xbe, const HostImage& host) {
    const uint32_t host_begin = address_of(host.base);
    const uint32_t host_end = host_begin + host.size;
    if (xbe.base_address() != host_begin || xbe.image_size() > host.size) {
        fatal("XBE wants %08X-%08X but the launcher reserved %08X-%08X", xbe.base_address(),
              xbe.base_address() + xbe.image_size(), host_begin, host_end);
    }
    for (const XbeSection& section : xbe.sections()) {
        if (section.virtual_address < host_begin || section.virtual_size > host_end - section.virtual_address) {
            fatal("XBE section %s lies outside the reserved range", section.name.c_str());
        }
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(host.base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(host.base + dos->e_lfanew);
    const std::vector<uint8_t> launcher_header(host.base, host.base + nt->OptionalHeader.SizeOfHeaders);

    DWORD old_protection = 0;
    if (!VirtualProtect(host.base, host.size, PAGE_EXECUTE_READWRITE, &old_protection)) {
        fatal("VirtualProtect on the launcher image failed (error %lu)", GetLastError());
    }
    // From here on the launcher's own headers and code are gone; nothing may return into them.
    std::memset(host.base, 0, host.size);

    const std::vector<uint8_t>& file = xbe.file();
    std::memcpy(host.base, file.data(), xbe.header_size());
    write_hybrid_pe_header(host.base, launcher_header);
    for (const XbeSection& section : xbe.sections()) {
        if (!section.preload()) continue;
        const uint32_t size = std::min(section.raw_size, section.virtual_size);
        std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(section.virtual_address)),
                    file.data() + section.raw_address, size);
    }
}

XbeTlsTemplate prepare_tls(const Xbe& xbe) {
    XbeTlsTemplate tls;
    const std::optional<XbeTlsDirectory> directory = xbe.tls();
    if (!directory) return tls;
    if (directory->raw_data_end > directory->raw_data_start) {
        tls.data = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(directory->raw_data_start));
        tls.data_size = directory->raw_data_end - directory->raw_data_start;
    }
    tls.zero_fill_size = directory->zero_fill_size;
    // XAPI overwrites the TLS index at startup with -(TLS area size / 4); see attach_xbox_thread_context.
    log_line("TLS: %u bytes of data, %u zero-filled, index at %08X", tls.data_size, tls.zero_fill_size,
             directory->index_address);
    return tls;
}

LONG WINAPI on_unhandled_exception(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    const CONTEXT* context = info->ContextRecord;
    log_line("");
    log_line("unhandled exception %08lX at %08X", record->ExceptionCode, address_of(record->ExceptionAddress));
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        log_line("  %s of address %08X", record->ExceptionInformation[0] ? "write" : "read",
                 static_cast<uint32_t>(record->ExceptionInformation[1]));
    }
    log_line("  eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX", context->Eax,
             context->Ebx, context->Ecx, context->Edx, context->Esi, context->Edi, context->Ebp, context->Esp);
    if (const XboxThreadContext* thread = current_xbox_thread()) {
        const auto* tls = reinterpret_cast<const uint32_t*>(thread->tls_area);
        const std::optional<XbeTlsDirectory> directory = g_xbe ? g_xbe->tls() : std::nullopt;
        const uint32_t index = directory && directory->index_address ? *guest<uint32_t>(directory->index_address) : 0;
        log_line("  xbox thread %u: tls area %08X = [%08X %08X %08X], tls index %u, irql %u", thread->thread_id,
                 address_of(tls), tls[0], tls[1], tls[2], index, current_irql());
    } else {
        log_line("  not an Xbox thread");
    }
    // ESP itself may be what went wrong: only read it if it points at committed memory.
    const auto* stack = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(context->Esp));
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(stack, &region, sizeof(region)) && region.State == MEM_COMMIT &&
        reinterpret_cast<const uint8_t*>(stack + 8) <=
            static_cast<const uint8_t*>(region.BaseAddress) + region.RegionSize &&
        !(region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        log_line("  stack: %08X %08X %08X %08X %08X %08X %08X %08X", stack[0], stack[1], stack[2], stack[3], stack[4],
                 stack[5], stack[6], stack[7]);
        log_game_return_addresses(stack);
    } else {
        log_line("  stack pointer %08lX is not readable", context->Esp);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// XAPI inspects the kernel image's own PE header at its fixed address (0x80010000) and looks for an "INIT"
// section to reclaim. A header with one ordinary section tells it there is nothing to do.
void map_kernel_image_header() {
    constexpr uint32_t kKernelImageBase = 0x80010000;
    constexpr uint32_t kNtHeadersOffset = 0x80;
    // Inside the physical memory window, in the pages reserved for the kernel.
    auto* base = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(kKernelImageBase));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = kNtHeadersOffset;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + kNtHeadersOffset);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    nt->FileHeader.NumberOfSections = 1;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER32);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    nt->OptionalHeader.ImageBase = kKernelImageBase;
    auto* section = IMAGE_FIRST_SECTION(nt);
    std::memcpy(section->Name, ".data", 5);
}

// MCPX devices the port does not emulate. DirectSound is replaced (dsound_hle.cpp), but its hardware stays
// mapped as plain memory for any leftover access; the network stack sees an adapter without a link.
void map_audio_placeholder() {
    struct Region {
        uint32_t base;
        uint32_t size;
    };
    constexpr Region kRegions[] = {
        {0xFE800000, 0x80000},  // APU
        {0xFEC00000, 0x1000},   // AC97
        {0xFEF00000, 0x1000},   // network controller: the port is offline, the network stack only initialises
    };
    for (const Region& region : kRegions) {
        if (!VirtualAlloc(reinterpret_cast<void*>(static_cast<uintptr_t>(region.base)), region.size,
                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
            fatal("cannot map the audio placeholder at %08X (error %lu)", region.base, GetLastError());
        }
    }
    const auto preset = [](uint32_t address, uint32_t value) {
        *reinterpret_cast<volatile uint32_t*>(static_cast<uintptr_t>(address)) = value;
    };
    preset(0xFE820010, 0x20);   // APU global processor counter: DirectSound waits for it to pass 0x20
    preset(0xFEC0012C, 0x100);  // AC97 global status: primary codec ready
}

// The Xbox has one CPU, and game code may rely on it (for example by never expecting two of its threads to run
// at the same time). MC3_MULTICORE=1 lifts the restriction.
void restrict_to_one_core() {
    if (GetEnvironmentVariableW(L"MC3_MULTICORE", nullptr, 0)) return;
    DWORD_PTR process_mask = 0, system_mask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) || !process_mask) return;
    const DWORD_PTR first_core = process_mask & (~process_mask + 1);
    SetProcessAffinityMask(GetCurrentProcess(), first_core);
    log_line("running on one CPU core (mask %08lX), like the Xbox", static_cast<unsigned long>(first_core));
}

// The game installs its own exception handlers (its crash screen exits to the dashboard), so faults would
// never reach on_unhandled_exception. A vectored handler sees them first; it only logs.
LONG WINAPI on_first_chance_exception(EXCEPTION_POINTERS* info) {
    constexpr DWORD kMsvcCppException = 0xE06D7363;
    constexpr DWORD kSetThreadName = 0x406D1388;
    static volatile long reported = 0;
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == STATUS_PRIVILEGED_INSTRUCTION && emulate_privileged_instruction(info->ContextRecord)) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2 &&
        nv2a_handle_access_violation(info->ContextRecord,
                                     static_cast<uint32_t>(info->ExceptionRecord->ExceptionInformation[1]))) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == kMsvcCppException || code == kSetThreadName || code == DBG_PRINTEXCEPTION_C ||
        code == DBG_PRINTEXCEPTION_WIDE_C) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (InterlockedIncrement(&reported) <= 20) {
        log_line("first-chance exception:");
        on_unhandled_exception(info);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

DWORD WINAPI xbox_main_thread(void* parameter) {
    const auto* start = static_cast<const MainThreadStart*>(parameter);
    attach_xbox_thread_context(start->tls, 0);
    log_line("jumping to the XBE entry point %08X", start->entry_point);
    using EntryPoint = void(__stdcall*)();
    reinterpret_cast<EntryPoint>(static_cast<uintptr_t>(start->entry_point))();
    log_line("the XBE entry point returned");
    return 0;
}

}  // namespace
}  // namespace mc3::pc

// Called by the launcher; never returns, because the launcher's code no longer exists once the XBE is mapped.
extern "C" __declspec(dllexport) __declspec(noreturn) void __cdecl mc3_run(void* reserved, unsigned long reserved_size) {
    using namespace mc3;
    using namespace mc3::pc;

    const HostImage host = host_image();
    const std::filesystem::path exe_directory = module_directory();
    log_open(exe_directory / "mc3port.log");
    SetUnhandledExceptionFilter(on_unhandled_exception);
    AddVectoredExceptionHandler(1, on_first_chance_exception);

    const uint32_t reserved_offset = address_of(reserved) - address_of(host.base);  // wraps if below the base
    if (reserved_offset > host.size || reserved_size > host.size - reserved_offset) {
        fatal("launcher reservation %08X+%08lX is not inside its own image", address_of(reserved), reserved_size);
    }

    const std::filesystem::path disc_path = find_disc_image(exe_directory);
    if (disc_path.empty()) fatal("Usage: mc3.exe <disc image .iso>\n\nNo .iso found next to mc3.exe.");
    const std::u8string disc_path_utf8 = disc_path.u8string();
    log_line("disc image: %s", reinterpret_cast<const char*>(disc_path_utf8.c_str()));

    std::string error;
    g_disc = XdvdfsImage::open(disc_path, &error);
    if (!g_disc) fatal("%s", error.c_str());
    const std::optional<XdvdfsImage::Entry> xbe_entry = g_disc->find("default.xbe");
    if (!xbe_entry) fatal("default.xbe not found on the disc");
    g_xbe = Xbe::parse(g_disc->read_all(*xbe_entry), &error);
    if (!g_xbe) fatal("default.xbe: %s", error.c_str());
    log_xbe(*g_xbe);

    map_xbe(*g_xbe, host);

    apply_hle_patches(*g_xbe);
    install_dsound_hle(*g_xbe);
    const FsPatchReport fs = patch_fs_reads(*g_xbe, g_exec_memory);
    log_line("fs: reads patched: %u (%u helpers), skipped: %u", fs.patched, fs.helpers, fs.skipped);

    register_all_kernel_functions();
    KernelThunkReport kernel;
    if (!install_kernel_thunks(*g_xbe, g_exec_memory, &kernel, &error)) fatal("%s", error.c_str());
    log_line("kernel imports: %u implemented, %u data, %u stubs", kernel.implemented, kernel.data,
             kernel.unimplemented);

    set_kernel_xbe(&*g_xbe);
    const std::filesystem::path hdd_directory = exe_directory / "xbox_hdd";
    configure_file_system(g_disc.get(), hdd_directory.c_str());
    log_line("hard disk partitions: %s", reinterpret_cast<const char*>(hdd_directory.u8string().c_str()));
    restrict_to_one_core();
    if (!map_physical_memory()) fatal("cannot map the Xbox physical memory window at %08X", kPhysicalMapBase);
    map_kernel_image_header();
    if (!start_nv2a()) fatal("cannot start the NV2A emulation");
    map_audio_placeholder();

    g_main_thread_start = MainThreadStart{g_xbe->entry_point(), prepare_tls(*g_xbe)};
    start_dispatcher_services(g_main_thread_start.tls);
    const XbeSection& text = g_xbe->sections().front();
    start_thread_watchdog(text.virtual_address, g_xbe->base_address() + g_xbe->image_size());
    // Committed, not just reserved: see kMinimumStackSize in kernel_threads.cpp.
    HANDLE thread = CreateThread(nullptr, kMainThreadStackSize, xbox_main_thread, &g_main_thread_start, 0, nullptr);
    if (!thread) fatal("CreateThread failed (error %lu)", GetLastError());
    CloseHandle(thread);

    // The XBE's startup thread ends once it has created the game's main thread; the game leaves through
    // HalReturnToFirmware, which exits the process.
    for (;;) Sleep(INFINITE);
}
