add_library(nova_inspection STATIC chain_report.cpp)
nova_apply_project_options(nova_inspection)
target_link_libraries(nova_inspection PUBLIC nova_storage)

# The freeze corpus is deliberately a pure, offline diagnostic.  It depends
# only on compiled-in consensus/primitives values and is not linked with node,
# transport, storage, wallet, RPC, or configuration code.
add_library(nova_freeze_corpus STATIC freeze_corpus.cpp)
nova_apply_project_options(nova_freeze_corpus)
target_link_libraries(nova_freeze_corpus PUBLIC nova_consensus nova_primitives)

add_executable(nova-freeze-corpus freeze_corpus_main.cpp)
nova_apply_project_options(nova-freeze-corpus)
target_link_libraries(nova-freeze-corpus PRIVATE nova_freeze_corpus nova_version)

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
