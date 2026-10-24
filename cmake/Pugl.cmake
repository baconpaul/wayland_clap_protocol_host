# Pugl (https://github.com/lv2/pugl), which plugins/pugl-demo needs. Pugl's Wayland backend is a
# work in progress on its `wayland` branch, which only makes toplevels: patches/pugl-wayland-embed.patch
# adds the embedding. Off by default, like Visage.
#
#   cmake -B build -DBUILD_PUGL_DEMO=ON                               # fetches and patches
#   cmake -B build -DBUILD_PUGL_DEMO=ON -DPUGL_SOURCE_DIR=../pugl     # a checkout, already patched
#
# Pugl builds with meson, so this builds its Wayland and OpenGL sources itself, as a static
# library which goes into the plugin.

option(BUILD_PUGL_DEMO "Build plugins/pugl-demo, with Pugl's Wayland backend" OFF)
set(PUGL_SOURCE_DIR "" CACHE PATH
    "A local Pugl checkout with patches/pugl-wayland-embed.patch applied (else fetched and patched)")
set(PUGL_GIT_REPOSITORY "https://github.com/lv2/pugl.git" CACHE STRING "Where to fetch Pugl")
# The commit of the `wayland` branch which the patch applies to. The branch is rebased, so this
# commit may go away.
set(PUGL_GIT_TAG "d28d56ba8d06a991f5e19fbd25d1684fdd6cf0cb"
    CACHE STRING "The commit of Pugl to fetch and patch")

if(BUILD_PUGL_DEMO)
  pkg_check_modules(PUGL_WAYLAND_DEPS REQUIRED IMPORTED_TARGET
    wayland-client wayland-cursor wayland-egl egl xkbcommon)
  find_package(OpenGL REQUIRED COMPONENTS OpenGL)

  if(PUGL_SOURCE_DIR)
    set(pugl_dir "${PUGL_SOURCE_DIR}")
  else()
    include(FetchContent)
    FetchContent_Declare(pugl GIT_REPOSITORY "${PUGL_GIT_REPOSITORY}" GIT_TAG "${PUGL_GIT_TAG}"
      PATCH_COMMAND git apply "${CMAKE_CURRENT_SOURCE_DIR}/patches/pugl-wayland-embed.patch"
      UPDATE_DISCONNECTED TRUE) # else the patch is applied again
    FetchContent_MakeAvailable(pugl) # no CMakeLists.txt there: only fetches
    set(pugl_dir "${pugl_SOURCE_DIR}")
  endif()

  add_library(pugl STATIC
    "${pugl_dir}/src/common.c"
    "${pugl_dir}/src/internal.c"
    "${pugl_dir}/src/wayland.c"
    "${pugl_dir}/src/wayland_gl.c"
    "${pugl_dir}/src/xdg-shell.c")
  set_target_properties(pugl PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99
                                        C_VISIBILITY_PRESET hidden)
  target_include_directories(pugl PUBLIC "${pugl_dir}/include")
  # Not exported from the plugin
  target_compile_definitions(pugl PUBLIC PUGL_STATIC "PUGL_API=" PRIVATE PUGL_INTERNAL)
  target_link_libraries(pugl PUBLIC PkgConfig::PUGL_WAYLAND_DEPS OpenGL::OpenGL)

  wayland_target_protocols(pugl
    stable/viewporter/viewporter.xml
    staging/fractional-scale/fractional-scale-v1.xml)

  # Pugl's own copy of the embedding protocol
  set(output_dir "${CMAKE_CURRENT_BINARY_DIR}/pugl-wayland-protocols")
  set(xml "${pugl_dir}/protocols/clap-surface-embedding-v1.xml")
  add_custom_command(
    OUTPUT "${output_dir}/clap-surface-embedding-v1-client-protocol.h"
           "${output_dir}/clap-surface-embedding-v1-protocol.c"
    COMMAND "${WAYLAND_SCANNER}" client-header "${xml}"
            "${output_dir}/clap-surface-embedding-v1-client-protocol.h"
    COMMAND "${WAYLAND_SCANNER}" private-code "${xml}"
            "${output_dir}/clap-surface-embedding-v1-protocol.c"
    DEPENDS "${xml}" VERBATIM)
  target_sources(pugl PRIVATE "${output_dir}/clap-surface-embedding-v1-client-protocol.h"
                              "${output_dir}/clap-surface-embedding-v1-protocol.c")

  add_subdirectory(plugins/pugl-demo)
endif()
