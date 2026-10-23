cmake_minimum_required(VERSION 3.21)
project(wayland-clap-protocol-host C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/clap/CMakeLists.txt")
  message(FATAL_ERROR "The submodules are missing: git submodule update --init")
endif()

# CLAP itself and the draft Wayland extensions (header only)
add_subdirectory(libs/clap EXCLUDE_FROM_ALL)
add_subdirectory(libs/clap-wayland-experiment EXCLUDE_FROM_ALL)

find_package(PkgConfig REQUIRED)
include(WaylandProtocols)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client>=1.22)

# The host
add_subdirectory(src)

# Example plugins of our own
add_subdirectory(plugins/wayland-shm-demo)

pkg_check_modules(SDL3 IMPORTED_TARGET sdl3>=3.2)
if(SDL3_FOUND)
  add_subdirectory(plugins/sdl3-demo)
else()
  message(STATUS "SDL3 not found (libsdl3-dev), plugins/sdl3-demo is not built")
endif()

# free-audio/clap-plugins, whose ImGui plugins (synth, check-wayland-embed, ...) were the first to
# implement the embedding. They take a while to build, so only on request:
#
#   cmake --build build --target example-plugins
#
# which gives build/example-plugins/clap-plugins.clap
include(ExampleClapPlugins)
