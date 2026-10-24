# The interface, which both the host and the plugin build into their binary
add_library(vst3-wayland-experiment STATIC source/iwaylandnestedcompositor.cpp)
target_include_directories(vst3-wayland-experiment PUBLIC include)
set_target_properties(vst3-wayland-experiment PROPERTIES POSITION_INDEPENDENT_CODE ON
                                                         CXX_VISIBILITY_PRESET hidden)
target_link_libraries(vst3-wayland-experiment PUBLIC vst3sdk-base)
