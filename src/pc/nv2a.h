#pragma once

#include <windows.h>

#include <cstdint>

namespace mc3::pc {

// Emulated NV2A GPU. The game's own Direct3D library runs unchanged: it programs the GPU registers at
// 0xFD000000 and fills push buffers in physical memory; this module plays the GPU's part.
//
//   - register space: every access traps and is replayed against a register model with the hardware's
//     semantics (write-1-to-clear interrupts, live timer and raster, DMA PUT wakes the GPU thread);
//   - push buffer: the GPU thread follows PUT and executes the methods it knows;
//   - vertical blank: 60 Hz interrupt to the driver's ISR, and the front buffer is shown in a window.
constexpr uint32_t kNv2aRegisterBase = 0xFD000000;
constexpr uint32_t kNv2aRegisterSize = 0x01000000;

bool start_nv2a();

// Called from the vectored exception handler; true if the fault was a register access and has been emulated.
bool nv2a_handle_access_violation(CONTEXT* context, uint32_t address);

// The game window, created on first use (its own thread runs the message loop). start_nv2a uses it too.
HWND nv2a_open_window();
// A function seeing the window's messages first (the launcher's UI); returning true means handled, with *result.
// It runs on the window's thread. nullptr removes it.
using WindowHook = bool (*)(HWND window, UINT message, WPARAM wparam, LPARAM lparam, LRESULT* result);
void nv2a_set_window_hook(WindowHook hook);
// Borderless full screen on the window's monitor, or back to the window.
void nv2a_set_full_screen(bool full_screen);

// Loads the window's icons from mc3.exe's resources. Must run before the XBE is mapped over the exe's image,
// which erases them.
void nv2a_load_icons();

// From AvSetDisplayMode: what the video encoder would scan out.
void nv2a_set_display_mode(uint32_t mode, uint32_t format, uint32_t pitch, uint32_t frame_buffer);

}  // namespace mc3::pc
