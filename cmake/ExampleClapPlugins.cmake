# The example-plugins target: fetches free-audio/clap-plugins at a commit which is known to embed
# with this host, builds its clap-plugins.clap, and copies it to ${CMAKE_BINARY_DIR}/example-plugins.

include(ExternalProject)

set(CLAP_PLUGINS_REPOSITORY "https://github.com/free-audio/clap-plugins.git"
    CACHE STRING "Where the example-plugins target gets free-audio/clap-plugins")
set(CLAP_PLUGINS_TAG "9fdaf3d667c25bb49ff7a04cd877491c48b56524"
    CACHE STRING "The commit of free-audio/clap-plugins which the example-plugins target builds")

set(_output_dir "${CMAKE_BINARY_DIR}/example-plugins")

ExternalProject_Add(example-plugins
  PREFIX "${CMAKE_BINARY_DIR}/clap-plugins"
  GIT_REPOSITORY "${CLAP_PLUGINS_REPOSITORY}"
  GIT_TAG "${CLAP_PLUGINS_TAG}"
  GIT_SUBMODULES_RECURSE TRUE
  GIT_SHALLOW FALSE
  UPDATE_DISCONNECTED TRUE
  # Single configuration, whatever generator we use ourselves
  CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release
  BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target clap-plugins
  INSTALL_COMMAND "${CMAKE_COMMAND}" -E make_directory "${_output_dir}"
          COMMAND "${CMAKE_COMMAND}" -E copy <BINARY_DIR>/plugins/clap-plugins.clap "${_output_dir}/"
  BUILD_BYPRODUCTS "${_output_dir}/clap-plugins.clap"
  EXCLUDE_FROM_ALL TRUE
  USES_TERMINAL_DOWNLOAD TRUE
  USES_TERMINAL_BUILD TRUE)
