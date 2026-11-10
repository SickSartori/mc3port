cmake_minimum_required(VERSION 3.25)
project(mc3port VERSION 0.3.5 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT everywhere: the runtime DLL must not depend on redistributables.
# Read when targets are created, so it has to come before them.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The configuration with symbols inlines like Release, so that what is measured and tried is what ships.
string(REPLACE "/Ob1" "/Ob2" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
string(REPLACE "/Ob1" "/Ob2" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")

# The shared runtime (private submodule, external/sick-xport): the Xbox kernel, memory, threads, the NV2A's
# registers and textures, paths, logs; xportcore is its portable part (discs, XBE, kernel exports).
include(external/sick-xport/xport.cmake)

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

    # SDL3 (zlib): only its gamepad side, for Xbox, PlayStation, Switch and other controllers (src/pc/gamepad.cpp).
    # Downloaded at configure time, pinned by version and hash, linked statically.
    include(FetchContent)
    FetchContent_Declare(SDL3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz
        URL_HASH SHA256=f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f
    )
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    foreach(subsystem AUDIO VIDEO HAPTIC POWER SENSOR DIALOG)
        set(SDL_${subsystem} OFF CACHE BOOL "" FORCE)
    endforeach()
    FetchContent_MakeAvailable(SDL3)

    # Runtime: maps the XBE, emulates the Xbox kernel and hosts the game's threads.
    add_library(mc3runtime SHARED
        ${XPORT_RUNTIME_SOURCES}
        src/pc/audio_mixer.cpp
        src/pc/dsound_hle.cpp
        src/pc/game_disc.cpp
        src/pc/hle_patches.cpp
        src/pc/hud.cpp
        src/pc/input_hle.cpp
        src/pc/kernel_io.cpp
        src/pc/kernel_threads.cpp
        src/pc/keyboard_bindings.cpp
        src/pc/launcher_art.cpp
        src/pc/launcher_ui.cpp
        src/pc/nv2a.cpp
        src/pc/nv2a_kelvin.cpp
        src/pc/renderer_game.cpp
        src/pc/runtime.cpp
        src/pc/settings.cpp
        res/launcher.rc
    )
    target_link_libraries(mc3runtime PRIVATE xportcore shell32 user32 gdi32 winmm ole32 xaudio2 d3d11 dxgi d3dcompiler dbghelp dwmapi bcrypt comdlg32 winhttp windowscodecs imgui mfplat mfreadwrite mfuuid shlwapi SDL3::SDL3-static)
    target_compile_definitions(mc3runtime PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
                               MC3PORT_VERSION="${PROJECT_VERSION}" ${MC3_VERSION_DEFINES})
    target_compile_options(mc3runtime PRIVATE /utf-8)
    target_include_directories(mc3runtime PRIVATE src res)
    # Not XPORT_DATA_WITH_RECOMPLAUNCHER: mc3port's RecompLauncher players have their data in Documents\mc3port.
    xport_identity(mc3runtime mc3port MC3PORT MC3_)
    # The GPU thread's hot path crosses files (methods, physical memory, renderer).
    set_target_properties(mc3runtime PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
                                                INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)

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

    # Tests: ctest --test-dir build/pc-x86 -C Release. They need no game file; disc_real uses MC3_TEST_DISC if set.
    include(CTest)
    if(BUILD_TESTING)
        function(mc3_test name)
            add_executable(${name} tests/${name}.cpp ${ARGN})
            target_link_libraries(${name} PRIVATE xportcore)
            target_include_directories(${name} PRIVATE src tests ${XPORT_DIR}/tests)
            target_compile_definitions(${name} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
            target_compile_options(${name} PRIVATE /utf-8)
            set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
            add_test(NAME ${name} COMMAND ${name})
        endfunction()
        mc3_test(test_direction_pace)
        mc3_test(test_audio_gains)
        mc3_test(test_launcher_style src/pc/settings.cpp ${XPORT_DIR}/src/pc/log.cpp)
        mc3_test(test_settings src/pc/settings.cpp ${XPORT_DIR}/src/pc/log.cpp)
        xport_add_tests(mc3port MC3PORT MC3_)
        mc3_test(test_game_disc src/pc/game_disc.cpp)
        target_link_libraries(test_game_disc PRIVATE bcrypt comdlg32 shell32 user32)
        add_test(NAME disc_real COMMAND test_game_disc real)
        set_tests_properties(disc_real PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()
