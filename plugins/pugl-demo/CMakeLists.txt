# A CLAP whose GUI is drawn by Pugl (OpenGL with EGL) into a surface embedded by the host's
# nested compositor. Pugl needs its Wayland backend with the embedding, see cmake/Pugl.cmake.
add_library(pugl-demo MODULE pugl-demo.cpp)
set_target_properties(pugl-demo PROPERTIES PREFIX "" SUFFIX ".clap"
                                           CXX_VISIBILITY_PRESET hidden)

target_compile_options(pugl-demo PRIVATE -Wall -Wextra -Wno-unused-parameter
                                         -Wno-missing-field-initializers)
target_link_libraries(pugl-demo PRIVATE
  clap
  clap-wayland-experiment
  pugl
  PkgConfig::WAYLAND_CLIENT)
