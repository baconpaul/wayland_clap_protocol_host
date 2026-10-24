# A CLAP whose GUI is drawn by Visage (bgfx and Vulkan) into a surface embedded by the host's
# nested compositor. Visage needs its Wayland backend, see cmake/Visage.cmake.
add_library(visage-demo MODULE visage-demo.cpp)
set_target_properties(visage-demo PROPERTIES PREFIX "" SUFFIX ".clap"
                                             CXX_VISIBILITY_PRESET hidden)

target_compile_options(visage-demo PRIVATE -Wall -Wextra -Wno-unused-parameter
                                           -Wno-missing-field-initializers)
target_link_libraries(visage-demo PRIVATE
  clap
  clap-wayland-experiment
  visage
  PkgConfig::WAYLAND_CLIENT)
