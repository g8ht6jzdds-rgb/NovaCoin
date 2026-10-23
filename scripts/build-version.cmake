# Refresh at every build, not only configure: HEAD and working-tree state can change.
set(NOVA_ARCHIVE_REVISION "" CACHE STRING "Declared source archive commit (no Git checkout)")
set(NOVA_ARCHIVE_TREE "" CACHE STRING "Declared source archive tree (no Git checkout)")
add_custom_target(nova_version_metadata
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DOUTPUT=${PROJECT_BINARY_DIR}/generated/build/version_data.hpp"
        "-DAPP_VERSION=${NOVA_VERSION}"
        "-DCOMPILER=${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}"
        "-DTARGET_PLATFORM=${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}"
        "-DBUILD_CONFIGURATION=$<CONFIG>"
        "-DBUILD_EPOCH=$ENV{SOURCE_DATE_EPOCH}"
        "-DARCHIVE_REVISION=${NOVA_ARCHIVE_REVISION}"
        "-DARCHIVE_TREE=${NOVA_ARCHIVE_TREE}"
        -P ${PROJECT_SOURCE_DIR}/scripts/write-version.cmake
    BYPRODUCTS ${PROJECT_BINARY_DIR}/generated/build/version_data.hpp
    VERBATIM)
add_library(nova_version INTERFACE)
add_dependencies(nova_version nova_version_metadata)
target_include_directories(nova_version INTERFACE
    ${PROJECT_SOURCE_DIR}/src ${PROJECT_BINARY_DIR}/generated)

# Offline metadata emitter: no daemon construction, credentials or network activity.
add_executable(nova-build-info ${PROJECT_SOURCE_DIR}/src/build/info.cpp)
target_link_libraries(nova-build-info PRIVATE nova_version nova_consensus)
nova_apply_project_options(nova-build-info)
add_custom_target(release-metadata
    COMMAND nova-build-info "${PROJECT_BINARY_DIR}/release-metadata.json"
    DEPENDS nova-build-info
    COMMENT "Writing deterministic public build metadata")
