#pragma once

// Shared between the modules that implement the emulated Xbox kernel (kernel_*.cpp).

#include <cstdint>

#include "core/xbe.h"
#include "core/xdvdfs.h"
#include "pc/kernel.h"
#include "pc/xbox_thread.h"
#include "pc/xbox_types.h"

namespace mc3::pc {

template <typename Function>
void register_kernel(uint32_t ordinal, Function* function) {
    register_kernel_function(ordinal, reinterpret_cast<const void*>(function));
}

void register_dispatcher_functions();
void register_thread_functions();
void register_memory_functions();
void register_system_functions();
void register_io_functions();

// Dispatcher (kernel_dispatcher.cpp). Objects are addresses of Xbox-layout dispatcher structures.
NTSTATUS wait_for_objects(uint32_t count, const uint32_t* objects, bool wait_all, const int64_t* timeout);
int32_t set_event(uint32_t event);
void signal_thread_object(uint32_t kthread);  // the thread has terminated
void start_dispatcher_services(const XbeTlsTemplate& tls);

// Live kernel variables exported to the game as data.
void* kernel_tick_count();
void* kernel_system_time();
void* kernel_interrupt_time();

// Converts an NT timeout (negative: relative, 100 ns units; positive: absolute system time) to milliseconds
// from now, clamped at 0. nullptr means infinite (returns -1).
int64_t timeout_to_milliseconds(const int64_t* timeout);

// Value of the LaunchDataPage kernel variable (a LAUNCH_DATA_PAGE*, 0 if none).
uint32_t kernel_launch_data_page();

// The XBE being run, for XeLoadSection.
void set_kernel_xbe(const Xbe* xbe);
const Xbe* kernel_xbe();

// Disc image and host directories backing the Xbox file system (kernel_io.cpp).
void configure_file_system(const XdvdfsImage* disc, const wchar_t* host_data_directory);

}  // namespace mc3::pc
