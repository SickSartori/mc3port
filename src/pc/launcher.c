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
        MessageBoxA(NULL, "Cannot load mc3runtime.dll: it must be next to mc3.exe.", "mc3port", MB_ICONERROR | MB_OK);
        ExitProcess(1);
    }
    run(g_xbe_reserve, XBE_RESERVE_SIZE);
    ExitProcess(1); /* not reached */
}
