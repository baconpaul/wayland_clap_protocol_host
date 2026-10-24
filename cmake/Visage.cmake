# Visage (https://github.com/VitalAudio/visage), which plugins/visage-demo needs. Upstream Visage
# has no Wayland backend: patches/visage-wayland.patch adds one. Off by default: Visage fetches
# bgfx and FreeType, and takes a while to build.
#
#   cmake -B build -DBUILD_VISAGE_DEMO=ON                                 # fetches and patches
#   cmake -B build -DBUILD_VISAGE_DEMO=ON -DVISAGE_SOURCE_DIR=../visage   # a checkout, already patched

option(BUILD_VISAGE_DEMO "Build plugins/visage-demo, with a patched Visage" OFF)
set(VISAGE_SOURCE_DIR "" CACHE PATH
    "A local Visage checkout with patches/visage-wayland.patch applied (else fetched and patched)")
set(VISAGE_GIT_REPOSITORY "https://github.com/VitalAudio/visage.git"
    CACHE STRING "Where to fetch Visage")
# The commit which the patch applies to
set(VISAGE_GIT_TAG "7654e32a84f95aacf9d15390272a47d2952d9565"
    CACHE STRING "The commit of Visage to fetch and patch")

if(BUILD_VISAGE_DEMO)
  # Visage is a static library, which goes into the plugin
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  set(VISAGE_WAYLAND ON CACHE BOOL "" FORCE)

  if(VISAGE_SOURCE_DIR)
    add_subdirectory("${VISAGE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/visage" EXCLUDE_FROM_ALL)
  else()
    include(FetchContent)
    FetchContent_Declare(visage GIT_REPOSITORY "${VISAGE_GIT_REPOSITORY}" GIT_TAG "${VISAGE_GIT_TAG}"
      PATCH_COMMAND git apply "${CMAKE_CURRENT_SOURCE_DIR}/patches/visage-wayland.patch"
      UPDATE_DISCONNECTED TRUE # else the patch is applied again
      EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(visage)
  endif()

  add_subdirectory(plugins/visage-demo)
endif()
