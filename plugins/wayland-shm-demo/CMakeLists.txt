# A CLAP whose GUI is drawn with libwayland-client and wl_shm only, embedded by the host's nested
# compositor
add_library(wayland-shm-demo MODULE wayland-shm-demo.cpp)
set_target_properties(wayland-shm-demo PROPERTIES PREFIX "" SUFFIX ".clap"
                                                  CXX_VISIBILITY_PRESET hidden)

# The embedding protocol refers to xdg-shell's interfaces
wayland_target_protocols(wayland-shm-demo stable/xdg-shell/xdg-shell.xml)
clap_wayland_experiment_target_protocols(wayland-shm-demo CLIENT)

target_compile_options(wayland-shm-demo PRIVATE -Wall -Wextra -Wno-unused-parameter
                                                -Wno-missing-field-initializers)
target_link_libraries(wayland-shm-demo PRIVATE
  clap
  clap-wayland-experiment
  PkgConfig::WAYLAND_CLIENT)
