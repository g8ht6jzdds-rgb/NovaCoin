add_library(nova_inspection STATIC chain_report.cpp)
nova_apply_project_options(nova_inspection)
target_link_libraries(nova_inspection PUBLIC nova_storage)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        OUTPUT_VARIABLE nova_inspect_revision OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT nova_inspect_revision)
        set(nova_inspect_revision "UNRECORDED")
    endif()
    set(NOVA_INSPECT_SOURCE_REVISION "${nova_inspect_revision}" CACHE STRING "Inspector build source commit")
    execute_process(COMMAND git rev-parse "HEAD^{tree}" WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        OUTPUT_VARIABLE nova_inspect_tree OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT nova_inspect_tree)
        set(nova_inspect_tree "UNRECORDED")
    endif()
    set(NOVA_INSPECT_SOURCE_TREE "${nova_inspect_tree}" CACHE STRING "Inspector build source tree")
    add_executable(nova-chain-inspect main.cpp snapshot.cpp)
    nova_apply_project_options(nova-chain-inspect)
    target_link_libraries(nova-chain-inspect PRIVATE nova_inspection)
    target_compile_definitions(nova-chain-inspect PRIVATE
        NOVA_INSPECT_SOURCE_REVISION="${NOVA_INSPECT_SOURCE_REVISION}"
        NOVA_INSPECT_SOURCE_TREE="${NOVA_INSPECT_SOURCE_TREE}")
endif()
