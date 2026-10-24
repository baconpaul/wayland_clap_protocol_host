# A VST3 whose view is drawn with libwayland-client and wl_shm only, embedded by the host's nested
# compositor through vst3_equiv/extension. The bundle is
#   wayland-shm-demo-vst3.vst3/Contents/<arch>-linux/wayland-shm-demo-vst3.so
add_library(wayland-shm-demo-vst3 MODULE wayland-shm-demo-vst3.cpp)

set(bundle "${CMAKE_CURRENT_BINARY_DIR}/wayland-shm-demo-vst3.vst3")
set_target_properties(wayland-shm-demo-vst3 PROPERTIES
  PREFIX ""
  CXX_VISIBILITY_PRESET hidden
  LIBRARY_OUTPUT_DIRECTORY "${bundle}/Contents/${CMAKE_SYSTEM_PROCESSOR}-linux")

# The embedding protocol refers to xdg-shell's interfaces
wayland_target_protocols(wayland-shm-demo-vst3 stable/xdg-shell/xdg-shell.xml)
clap_wayland_experiment_target_protocols(wayland-shm-demo-vst3 CLIENT)

target_compile_options(wayland-shm-demo-vst3 PRIVATE -Wall -Wextra -Wno-unused-parameter
                                                     -Wno-missing-field-initializers)
target_link_libraries(wayland-shm-demo-vst3 PRIVATE
  vst3-wayland-experiment
  vst3sdk-plugin
  PkgConfig::WAYLAND_CLIENT)
