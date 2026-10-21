# Refresh at every build, not only configure: HEAD and working-tree state can change.
set(NOVA_ARCHIVE_REVISION "" CACHE STRING "Declared source archive commit (no Git checkout)")
set(NOVA_ARCHIVE_TREE "" CACHE STRING "Declared source archive tree (no Git checkout)")
add_custom_target(nova_version_metadata
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DOUTPUT=${PROJECT_BINARY_DIR}/generated/build/version_data.hpp"
        "-DAPP_VERSION=${PROJECT_VERSION}"
        "-DARCHIVE_REVISION=${NOVA_ARCHIVE_REVISION}"
        "-DARCHIVE_TREE=${NOVA_ARCHIVE_TREE}"
        -P ${PROJECT_SOURCE_DIR}/scripts/write-version.cmake
    BYPRODUCTS ${PROJECT_BINARY_DIR}/generated/build/version_data.hpp
    VERBATIM)
add_library(nova_version INTERFACE)
add_dependencies(nova_version nova_version_metadata)
target_include_directories(nova_version INTERFACE
    ${PROJECT_SOURCE_DIR}/src ${PROJECT_BINARY_DIR}/generated)
