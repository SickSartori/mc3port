cmake_minimum_required(VERSION 3.25)
project(mc3port LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT everywhere: the runtime DLL must not depend on redistributables.
# Read when targets are created, so it has to come before them.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Portable core: disc image and XBE parsing, kernel export table.
# No platform headers here: the Xbox 360 target reuses it as is.
add_library(mc3core STATIC
    src/core/kernel_exports.cpp
    src/core/xbe.cpp
    src/core/xdvdfs.cpp
)
target_include_directories(mc3core PUBLIC src)

if(WIN32)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR "The PC build must be 32-bit (x86): the game's x86 code runs natively in the process. "
                            "Configure with -A Win32 or use the 'pc-x86' preset.")
    endif()

    # Runtime: maps the XBE, emulates the Xbox kernel and hosts the game's threads.
    add_library(mc3runtime SHARED
        src/pc/exec_memory.cpp
        src/pc/fs_patch.cpp
        src/pc/handles.cpp
        src/pc/kernel.cpp
        src/pc/kernel_dispatcher.cpp
        src/pc/kernel_io.cpp
        src/pc/kernel_memory.cpp
        src/pc/kernel_system.cpp
        src/pc/kernel_threads.cpp
        src/pc/log.cpp
        src/pc/runtime.cpp
        src/pc/xbox_thread.cpp
    )
    target_link_libraries(mc3runtime PRIVATE mc3core shell32 user32 winmm)
    target_compile_definitions(mc3runtime PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

    # Launcher: linked at the XBE base (0x10000) so Windows reserves the game's address range for us.
    # It has no CRT; runtime checks and security cookies would pull one in.
    string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    add_executable(mc3 src/pc/launcher.c)
    target_compile_options(mc3 PRIVATE /GS-)
    target_link_libraries(mc3 PRIVATE kernel32)
    target_link_options(mc3 PRIVATE
        /NODEFAULTLIB /ENTRY:LauncherEntry /SUBSYSTEM:CONSOLE
        /BASE:0x10000 /FIXED /DYNAMICBASE:NO /LARGEADDRESSAWARE
        /SAFESEH:NO /INCREMENTAL:NO
    )
    add_dependencies(mc3 mc3runtime)
endif()
