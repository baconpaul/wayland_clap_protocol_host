include(WaylandProtocols)

pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client>=1.22 wayland-server>=1.22)
pkg_check_modules(LIBDECOR REQUIRED IMPORTED_TARGET libdecor-0)
# For the title bars which the nested compositor draws around the plugins' windows
pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo cairo)
find_package(Threads REQUIRED)

# The nested compositor and the host's windows, which know nothing of the plugin format: the CLAP
# host here and the VST3 host in vst3_equiv/host share them
add_library(wayland-embedding-host STATIC
  wayland-window.cpp
  nested-compositor.cpp
  decorations.cpp)
target_include_directories(wayland-embedding-host PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# Used toward the system compositor only
wayland_target_protocols(wayland-embedding-host
  staging/cursor-shape/cursor-shape-v1.xml
  unstable/tablet/tablet-unstable-v2.xml) # cursor-shape refers to it

# Also offered by the nested compositor to the plugins
wayland_target_protocols(wayland-embedding-host SERVER
  stable/xdg-shell/xdg-shell.xml
  stable/linux-dmabuf/linux-dmabuf-v1.xml
  stable/viewporter/viewporter.xml
  staging/fractional-scale/fractional-scale-v1.xml
  unstable/xdg-decoration/xdg-decoration-unstable-v1.xml)

# clap_surface_embedding_v1, which the nested compositor implements
clap_wayland_experiment_target_protocols(wayland-embedding-host SERVER)

target_compile_options(wayland-embedding-host PRIVATE -Wall -Wextra -Wno-unused-parameter
                                                      -Wno-missing-field-initializers)
target_link_libraries(wayland-embedding-host PUBLIC
  PkgConfig::WAYLAND
  PkgConfig::LIBDECOR
  PkgConfig::PANGOCAIRO
  Threads::Threads)

add_executable(clap-wayland-host
  main.cpp
  host.cpp)

target_compile_options(clap-wayland-host PRIVATE -Wall -Wextra -Wno-unused-parameter
                                                 -Wno-missing-field-initializers)
target_link_libraries(clap-wayland-host PRIVATE
  wayland-embedding-host
  clap
  clap-wayland-experiment
  ${CMAKE_DL_LIBS})
