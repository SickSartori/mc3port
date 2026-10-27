cmake_minimum_required(VERSION 3.25)
project(mc3port VERSION 0.2.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT everywhere: the runtime DLL must not depend on redistributables.
# Read when targets are created, so it has to come before them.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Portable core: disc image and XBE parsing, kernel export table.
# No platform headers here.
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

    # Version numbers for the version resource (res/version.rc) of mc3.exe and mc3runtime.dll.
    set(MC3_VERSION_DEFINES MC3_VERSION_MAJOR=${PROJECT_VERSION_MAJOR} MC3_VERSION_MINOR=${PROJECT_VERSION_MINOR}
                            MC3_VERSION_PATCH=${PROJECT_VERSION_PATCH})

    # Dear ImGui (MIT, third_party/imgui): the launcher's interface.
    add_library(imgui STATIC
        third_party/imgui/imgui.cpp
        third_party/imgui/imgui_draw.cpp
        third_party/imgui/imgui_tables.cpp
        third_party/imgui/imgui_widgets.cpp
        third_party/imgui/backends/imgui_impl_dx11.cpp
        third_party/imgui/backends/imgui_impl_win32.cpp
    )
    target_include_directories(imgui PUBLIC third_party/imgui)
    # The launcher polls the controllers itself (every port, not only the first): see Launcher::poll_gamepads.
    target_compile_definitions(imgui PRIVATE NOMINMAX IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)

    # Runtime: maps the XBE, emulates the Xbox kernel and hosts the game's threads.
    add_library(mc3runtime SHARED
        src/pc/audio_mixer.cpp
        src/pc/dsound_hle.cpp
        src/pc/exec_memory.cpp
        src/pc/fs_patch.cpp
        src/pc/game_disc.cpp
        src/pc/game_pause.cpp
        src/pc/handles.cpp
        src/pc/hle_patches.cpp
        src/pc/input_hle.cpp
        src/pc/kernel.cpp
        src/pc/kernel_crypto.cpp
        src/pc/kernel_dispatcher.cpp
        src/pc/kernel_io.cpp
        src/pc/kernel_memory.cpp
        src/pc/kernel_system.cpp
        src/pc/kernel_threads.cpp
        src/pc/keyboard_bindings.cpp
        src/pc/launcher_art.cpp
        src/pc/launcher_music.cpp
        src/pc/launcher_ui.cpp
        src/pc/log.cpp
        src/pc/mmio.cpp
        src/pc/nv2a.cpp
        src/pc/nv2a_combiner.cpp
        src/pc/nv2a_kelvin.cpp
        src/pc/nv2a_texture.cpp
        src/pc/nv2a_vsh.cpp
        src/pc/paths.cpp
        src/pc/physical_memory.cpp
        src/pc/privileged.cpp
        src/pc/renderer_d3d11.cpp
        src/pc/runtime.cpp
        src/pc/settings.cpp
        src/pc/xbox_thread.cpp
        res/launcher.rc
    )
    target_link_libraries(mc3runtime PRIVATE mc3core shell32 user32 gdi32 winmm ole32 xaudio2 d3d11 dxgi d3dcompiler dbghelp dwmapi bcrypt comdlg32 winhttp urlmon imgui mfplat mfreadwrite mfuuid shlwapi)
    target_compile_definitions(mc3runtime PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
                               MC3PORT_VERSION="${PROJECT_VERSION}" ${MC3_VERSION_DEFINES})
    target_compile_options(mc3runtime PRIVATE /utf-8)

    # Launcher: linked at the XBE base (0x10000) so Windows reserves the game's address range for us.
    # It has no CRT; runtime checks and security cookies would pull one in.
    string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    add_executable(mc3 src/pc/launcher.c res/mc3.rc)
    target_compile_definitions(mc3 PRIVATE ${MC3_VERSION_DEFINES} MC3_RC_APP=1)
    target_compile_options(mc3 PRIVATE /GS-)
    target_link_libraries(mc3 PRIVATE kernel32 user32)
    target_link_options(mc3 PRIVATE
        /NODEFAULTLIB /ENTRY:LauncherEntry /SUBSYSTEM:WINDOWS
        /BASE:0x10000 /FIXED /DYNAMICBASE:NO /LARGEADDRESSAWARE
        /SAFESEH:NO /INCREMENTAL:NO
    )
    add_dependencies(mc3 mc3runtime)
endif()
