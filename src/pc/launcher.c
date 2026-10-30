/* mc3.exe: linked at the XBE base address (0x10000) with a large uninitialised section, so Windows itself
 * reserves the address range the game expects before anything else (heaps, stacks, DLLs) can land there.
 *
 * It then hands over to mc3runtime.dll, which overwrites this whole image with the XBE. No CRT, no globals
 * other than the reservation, and nothing may run here after mc3_run is called. */

#include <windows.h>

/* Covers the XBE image (0x57A280 bytes for MC3) with room for sections loaded on demand. */
#define XBE_RESERVE_SIZE 0x01000000u

#pragma bss_seg(".xbe")
unsigned char g_xbe_reserve[XBE_RESERVE_SIZE];
#pragma bss_seg()

typedef void(__cdecl* Mc3RunFunction)(void* reserved, unsigned long reserved_size);

void __cdecl LauncherEntry(void) {
    HMODULE runtime = LoadLibraryW(L"mc3runtime.dll");
    Mc3RunFunction run = runtime ? (Mc3RunFunction)GetProcAddress(runtime, "mc3_run") : NULL;
    if (!run) {
        /* The DLL may be missing, quarantined by an antivirus, or one of the Windows components it needs may be
         * (Media Foundation is absent from Windows N editions): the error says which. wsprintf and FormatMessage
         * need no C runtime. */
        DWORD error = GetLastError();
        char reason[256];
        char message[768];
        if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, error, 0, reason,
                            sizeof(reason), NULL)) {
            reason[0] = 0;
        }
        wsprintfA(message,
                  "Cannot load mc3runtime.dll (error %lu). %s\n\n"
                  "The file must be next to mc3.exe. If it is there, a Windows component is missing: on Windows N "
                  "editions install the Media Feature Pack (Settings > Apps > Optional features). An antivirus may "
                  "also have quarantined the file.",
                  error, reason);
        MessageBoxA(NULL, message, "mc3port", MB_ICONERROR | MB_OK);
        ExitProcess(1);
    }
    run(g_xbe_reserve, XBE_RESERVE_SIZE);
    ExitProcess(1); /* not reached */
}
