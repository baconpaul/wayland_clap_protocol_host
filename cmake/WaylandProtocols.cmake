# wayland_target_protocols(<target> [SERVER] <xml>...)
#
# Generates the client header and glue code for each protocol with wayland-scanner and adds
# them to <target>; with SERVER, also the server header, for a target which implements the
# protocol too (a nested compositor). The xml paths are relative to the wayland-protocols data
# directory, for example stable/xdg-shell/xdg-shell.xml.
#
# The headers are named <protocol>-client-protocol.h and <protocol>-server-protocol.h.

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols>=1.32)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

function(wayland_target_protocols target)
  cmake_parse_arguments(PARSE_ARGV 1 arg "SERVER" "" "")

  set(output_dir "${CMAKE_CURRENT_BINARY_DIR}/${target}-wayland-protocols")
  file(MAKE_DIRECTORY "${output_dir}")

  foreach(rel ${arg_UNPARSED_ARGUMENTS})
    get_filename_component(name "${rel}" NAME_WE)
    set(xml "${WAYLAND_PROTOCOLS_DIR}/${rel}")
    set(outputs "${output_dir}/${name}-client-protocol.h" "${output_dir}/${name}-protocol.c")
    set(commands
      COMMAND "${WAYLAND_SCANNER}" client-header "${xml}" "${output_dir}/${name}-client-protocol.h"
      COMMAND "${WAYLAND_SCANNER}" private-code "${xml}" "${output_dir}/${name}-protocol.c")
    if(arg_SERVER)
      list(APPEND outputs "${output_dir}/${name}-server-protocol.h")
      list(APPEND commands
        COMMAND "${WAYLAND_SCANNER}" server-header "${xml}" "${output_dir}/${name}-server-protocol.h")
    endif()

    add_custom_command(OUTPUT ${outputs} ${commands} DEPENDS "${xml}" VERBATIM)
    target_sources(${target} PRIVATE ${outputs})
  endforeach()

  target_include_directories(${target} PRIVATE "${output_dir}")
endfunction()
