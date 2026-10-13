cmake_minimum_required(VERSION 3.20)
project(GGDPVR LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: no extra runtime DLLs next to the game.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_options(/W4 /wd4100 /wd4201 /utf-8 /Zi)
add_link_options(/DEBUG /OPT:REF /OPT:ICF /PDBALTPATH:%_PDB%)  # PDBALTPATH: only the .pdb file name in the binaries, not the build path
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

# --- MinHook (vendored source) ---
add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)
target_compile_options(minhook PRIVATE /W0)

# --- OpenXR loader (vendored KhronosGroup/OpenXR-SDK, static) ---
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/OpenXR-SDK EXCLUDE_FROM_ALL)
# The SDK forces the dynamic CRT for the static loader; GGDPVR uses the static CRT everywhere.
set_target_properties(openxr_loader PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- dinput8.dll proxy loader ---
add_library(dinput8 SHARED loader/dinput8_proxy.cpp loader/dinput8.def)
set_target_properties(dinput8 PROPERTIES OUTPUT_NAME dinput8)

# --- GGDPVR core ---
add_library(GGDPVR SHARED
    src/dllmain.cpp
    src/core/log.cpp
    src/core/config.cpp
    src/core/game.cpp
    src/core/hooks.cpp
    src/core/settings_guard.cpp
    src/diag/rr_probe.cpp
    src/diag/vf_trace.cpp
    src/diag/thread_stack.cpp
    src/diag/renderdoc_capture.cpp
    src/fix/fix_deferred_cleanup.cpp
    src/ue3/objects.cpp
    src/ue3/process_event.cpp
    src/ue3/console.cpp
    src/ue3/render_cmd.cpp
    src/xr/present_hook.cpp
    src/xr/xr_screen.cpp
    src/vr/head_tracking.cpp
    src/vr/render_res.cpp
    src/vr/ui_layer.cpp
    src/fix/fix_shader_nan.cpp
    src/xr/depth_probe.cpp
    src/vr/vpad.cpp
    src/vr/headset_audio.cpp
    src/vr/menu_pointer.cpp
    src/vr/gfx_vars.cpp
    src/vr/controllers.cpp
    src/xr/xr_input.cpp)
target_link_libraries(GGDPVR PRIVATE minhook openxr_loader)
target_include_directories(GGDPVR PRIVATE third_party/renderdoc)

# --- GGDPVR Setup (the players' setup program; the loader, the mod and the default ini are embedded) ---
file(STRINGS src/dllmain.cpp _ggdpvr_version_line REGEX "#define GGDPVR_VERSION")
string(REGEX MATCH "\"([0-9]+)\.([0-9]+)\.([0-9]+)" _ggdpvr_version_match "${_ggdpvr_version_line}")
set(GGDPVR_VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
set(GGDPVR_VERSION_COMMA "${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3},0")
add_executable(GGDPVR-Setup WIN32
    package/setup_app/setup_main.cpp
    package/setup_app/setup.rc
    package/setup_app/setup.manifest)
target_compile_definitions(GGDPVR-Setup PRIVATE UNICODE _UNICODE
    GGDPVR_SETUP_VERSION="${GGDPVR_VERSION}" GGDPVR_VER_COMMA=${GGDPVR_VERSION_COMMA})
target_include_directories(GGDPVR-Setup PRIVATE ${CMAKE_BINARY_DIR} config package/setup_app/res package/setup_app)
target_link_libraries(GGDPVR-Setup PRIVATE gdiplus shlwapi ole32 shell32 uuid)
# Windows DLLs that the setup imports come from System32 only (never from the folder it was downloaded to)
target_link_options(GGDPVR-Setup PRIVATE /DEPENDENTLOADFLAG:0x800)
add_dependencies(GGDPVR-Setup dinput8 GGDPVR)
set_source_files_properties(package/setup_app/setup.rc PROPERTIES OBJECT_DEPENDS
    "${CMAKE_BINARY_DIR}/dinput8.dll;${CMAKE_BINARY_DIR}/GGDPVR.dll;${CMAKE_SOURCE_DIR}/config/ggdpvr.ini;${CMAKE_SOURCE_DIR}/package/setup_app/res/logo.png;${CMAKE_SOURCE_DIR}/package/setup_app/res/app.ico")
