# The VST3 host, on the nested compositor and windows which src/ shares with the CLAP host
add_executable(vst3-wayland-host
  main.cpp
  vst3-host.cpp)

target_compile_options(vst3-wayland-host PRIVATE -Wall -Wextra -Wno-unused-parameter
                                                 -Wno-missing-field-initializers)
target_link_libraries(vst3-wayland-host PRIVATE
  wayland-embedding-host
  vst3-wayland-experiment
  vst3sdk-hosting)
