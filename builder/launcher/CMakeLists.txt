# The Wonderpark launcher (one-time builder): a small native window,
# built on its own by tools/build_launcher.py. It contains no game code or
# data, so it can be shipped as is.
cmake_minimum_required(VERSION 3.20)
project(WonderparkLauncher CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static runtime: the launcher runs with nothing else installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(WonderparkLauncher WIN32 launcher.cpp mods.cpp launcher.rc)
target_compile_definitions(WonderparkLauncher PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
  target_compile_options(WonderparkLauncher PRIVATE /W3 /utf-8)
endif()
target_link_libraries(WonderparkLauncher PRIVATE d2d1 dwrite d3d11 dxgi dxguid windowscodecs ole32 shell32 shlwapi uuid)
