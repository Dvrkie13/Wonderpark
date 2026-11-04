cmake_minimum_required(VERSION 3.20)
project(Wonderpark C)

# Built on the player's PC after tools/recomp.py has written generated/.
# Nothing game-derived is in the repo; generated/ and build/ are git-ignored.

set(CMAKE_C_STANDARD 11)
set(WP_GAME_ID S2LE01)
set(WP_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated/${WP_GAME_ID}_generated"
    CACHE PATH "DolRecomp output for ${WP_GAME_ID}")
set(WP_DOLRECOMP_DIR "${CMAKE_SOURCE_DIR}/tools/dolrecomp")

if(NOT EXISTS "${WP_GENERATED_DIR}/${WP_GAME_ID}.h")
    message(FATAL_ERROR "No recompiled code in ${WP_GENERATED_DIR}. Run: python tools/recomp.py")
endif()

file(GLOB WP_CHUNKS CONFIGURE_DEPENDS "${WP_GENERATED_DIR}/chunks/*.c")
list(LENGTH WP_CHUNKS WP_CHUNK_COUNT)
message(STATUS "Wonderpark: ${WP_CHUNK_COUNT} recompiled chunks")

# DolRecomp's CPU helpers (GPL-3.0-or-later, from the submodule).
add_library(wp_dolrecomp_cpu STATIC "${WP_DOLRECOMP_DIR}/src/cpu/cpu.c")
target_include_directories(wp_dolrecomp_cpu PUBLIC "${WP_DOLRECOMP_DIR}/src")

# The recompiled game code.
add_library(wp_generated STATIC ${WP_CHUNKS})
target_include_directories(wp_generated PUBLIC "${WP_GENERATED_DIR}")
target_link_libraries(wp_generated PUBLIC wp_dolrecomp_cpu)

file(GLOB WP_RUNTIME_SOURCES CONFIGURE_DEPENDS
    runtime/core/*.c runtime/hw/*.c runtime/hle/*.c runtime/ios/*.c
    runtime/gpu/*.c runtime/host/*.c "${WP_GENERATED_DIR}/wp_hooks.c")
add_executable(Wonderpark ${WP_RUNTIME_SOURCES})
target_include_directories(Wonderpark PRIVATE runtime/core runtime/gpu runtime/host)
target_link_libraries(Wonderpark PRIVATE wp_generated)
if(WIN32)
    target_link_libraries(Wonderpark PRIVATE opengl32 gdi32 user32 winmm shell32 ole32 dwmapi)
endif()

if(MSVC)
    # Generated code: no warnings, big switch tables, parallel compile.
    target_compile_options(wp_generated PRIVATE /w /MP /bigobj)
    target_compile_definitions(wp_dolrecomp_cpu PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(Wonderpark PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(wp_generated PRIVATE -w)
endif()
