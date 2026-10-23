# A CLAP whose GUI is drawn by SDL3 into a surface embedded by the host's nested compositor
add_library(sdl3-demo MODULE sdl3-demo.cpp)
set_target_properties(sdl3-demo PROPERTIES PREFIX "" SUFFIX ".clap"
                                           CXX_VISIBILITY_PRESET hidden)

# The embedding protocol refers to xdg-shell's interfaces
wayland_target_protocols(sdl3-demo stable/xdg-shell/xdg-shell.xml)
clap_wayland_experiment_target_protocols(sdl3-demo CLIENT)

target_compile_options(sdl3-demo PRIVATE -Wall -Wextra -Wno-unused-parameter
                                         -Wno-missing-field-initializers)
target_link_libraries(sdl3-demo PRIVATE
  clap
  clap-wayland-experiment
  PkgConfig::SDL3
  PkgConfig::WAYLAND_CLIENT)
