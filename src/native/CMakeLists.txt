cmake_minimum_required(VERSION 3.22)
project(PirateSignalsNative LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PIRATE_SIGNALS_DEPS_DIR "" CACHE PATH "Directory containing imgui and kiero checkouts")
set(UE4SS_IMPORT_LIB "" CACHE FILEPATH "Path to a UE4SS import library compatible with the deployed build")

set(IMGUI_DIR "${PIRATE_SIGNALS_DEPS_DIR}/imgui" CACHE PATH "Dear ImGui source directory")
set(KIERO_DIR "${PIRATE_SIGNALS_DEPS_DIR}/kiero" CACHE PATH "Kiero source directory")

foreach(required IN ITEMS
    "${IMGUI_DIR}/imgui.cpp"
    "${KIERO_DIR}/kiero.cpp"
    "${KIERO_DIR}/minhook/src/hook.c"
    "${UE4SS_IMPORT_LIB}")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "Required build dependency is missing: ${required}")
    endif()
endforeach()

add_library(PirateSignalsNative SHARED
    src/dllmain.cpp
    "${KIERO_DIR}/kiero.cpp"
    "${KIERO_DIR}/minhook/src/buffer.c"
    "${KIERO_DIR}/minhook/src/hook.c"
    "${KIERO_DIR}/minhook/src/trampoline.c"
    "${KIERO_DIR}/minhook/src/hde/hde64.c"
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx12.cpp"
)

target_compile_definitions(PirateSignalsNative PRIVATE
    KIERO_INCLUDE_D3D12=1
    KIERO_USE_MINHOOK=1
    IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_include_directories(PirateSignalsNative PRIVATE
    src
    "${KIERO_DIR}"
    "${KIERO_DIR}/minhook/include"
    "${KIERO_DIR}/minhook/src"
    "${KIERO_DIR}/minhook/src/hde"
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
)

target_link_libraries(PirateSignalsNative PRIVATE
    "${UE4SS_IMPORT_LIB}"
    d3d12
    dxgi
    d3dcompiler
    user32
    imm32
)

set_target_properties(PirateSignalsNative PROPERTIES
    OUTPUT_NAME "main"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/output"
)
