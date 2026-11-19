# The Wonderpark launcher (one-time builder): a small native window,
# built on its own by tools/build_launcher.py. It contains no game code or
# data, so it can be shipped as is.
cmake_minimum_required(VERSION 3.20)
project(WonderparkLauncher C CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static runtime: the launcher runs with nothing else installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Controllers through SDL3 (zlib license; the tools/sdl3 submodule), as the
# game reads them: static, only the parts used.
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
foreach(_sdl_off AUDIO VIDEO GPU RENDER CAMERA POWER DIALOG TRAY)
    set(SDL_${_sdl_off} OFF CACHE BOOL "" FORCE)
endforeach()
foreach(_sdl_on JOYSTICK HAPTIC HIDAPI SENSOR)
    set(SDL_${_sdl_on} ON CACHE BOOL "" FORCE)
endforeach()
add_subdirectory(../../tools/sdl3 sdl3 EXCLUDE_FROM_ALL)

# The controller icons (CC BY-SA 4.0), the pad's type and the pads
# themselves, shared with the game.
add_executable(WonderparkLauncher WIN32 launcher.cpp mods.cpp saves.cpp wii_save.c hdpack.cpp launcher.rc
               ../../runtime/host/pad_icons_core.cpp ../../runtime/host/pad_icons.rc
               ../../runtime/host/pad_sdl.c ../../runtime/gpu/gx_texture.c)
# The HD Pack decodes the disc's textures with the game's own decoders
# (gx_texture.c, whose header reaches the runtime's and DolRecomp's).
set_source_files_properties(../../runtime/gpu/gx_texture.c PROPERTIES INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/gpu;${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/core;${CMAKE_CURRENT_SOURCE_DIR}/../../tools/dolrecomp/src")
target_compile_definitions(WonderparkLauncher PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(WonderparkLauncher PRIVATE ../../runtime/host)
if(MSVC)
  target_compile_options(WonderparkLauncher PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W3> /utf-8)
endif()
target_link_libraries(WonderparkLauncher PRIVATE SDL3::SDL3-static d2d1 dwrite d3d11 dxgi dxguid windowscodecs ole32 shell32
                      shlwapi uuid setupapi bcrypt urlmon)
