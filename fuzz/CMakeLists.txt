if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "NOVA_BUILD_FUZZERS requires Clang and libFuzzer support.")
endif()

function(nova_add_fuzzer target source)
    add_executable(${target} ${source})
    nova_apply_project_options(${target})
    target_compile_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
endfunction()

nova_add_fuzzer(fuzz_deserializer deserializer_fuzz.cpp)
target_link_libraries(fuzz_deserializer PRIVATE nova_primitives nova_consensus)

nova_add_fuzzer(fuzz_transaction_deserializer transaction_deserializer_fuzz.cpp)
target_link_libraries(fuzz_transaction_deserializer PRIVATE nova_primitives)

nova_add_fuzzer(fuzz_block_deserializer block_deserializer_fuzz.cpp)
target_link_libraries(fuzz_block_deserializer PRIVATE nova_primitives)

nova_add_fuzzer(fuzz_network_message network_message_fuzz.cpp)
target_link_libraries(fuzz_network_message PRIVATE nova_net)

nova_add_fuzzer(fuzz_script_signature script_signature_fuzz.cpp)
target_link_libraries(fuzz_script_signature PRIVATE nova_crypto)

nova_add_fuzzer(fuzz_address_parser address_parser_fuzz.cpp)
target_link_libraries(fuzz_address_parser PRIVATE nova_explorer)

nova_add_fuzzer(fuzz_compact_target compact_target_fuzz.cpp)
target_link_libraries(fuzz_compact_target PRIVATE nova_consensus)

nova_add_fuzzer(fuzz_peer_discovery peer_discovery_fuzz.cpp)
target_link_libraries(fuzz_peer_discovery PRIVATE nova_node)

nova_add_fuzzer(fuzz_sync_admission sync_admission_fuzz.cpp)
target_link_libraries(fuzz_sync_admission PRIVATE nova_node)

nova_add_fuzzer(fuzz_body_request_recovery body_request_recovery_fuzz.cpp)
target_link_libraries(fuzz_body_request_recovery PRIVATE nova_node)

nova_add_fuzzer(fuzz_bootstrap_reader bootstrap_reader_fuzz.cpp)
target_link_libraries(fuzz_bootstrap_reader PRIVATE nova_node)

nova_add_fuzzer(fuzz_offline_inspection offline_inspection_fuzz.cpp)
target_link_libraries(fuzz_offline_inspection PRIVATE nova_inspection)

add_custom_target(nova_fuzzers DEPENDS
    fuzz_wallet_import
    fuzz_rpc
    fuzz_journal
    fuzz_offline_inspection
    fuzz_deserializer
    fuzz_transaction_deserializer
    fuzz_block_deserializer
    fuzz_network_message
    fuzz_script_signature
    fuzz_address_parser
    fuzz_compact_target
    fuzz_peer_discovery
    fuzz_sync_admission
    fuzz_body_request_recovery
    fuzz_bootstrap_reader)

nova_add_fuzzer(fuzz_journal journal_fuzz.cpp)
target_link_libraries(fuzz_journal PRIVATE nova_storage nova_consensus)

nova_add_fuzzer(fuzz_rpc rpc_fuzz.cpp)
target_link_libraries(fuzz_rpc PRIVATE nova_rpc)

nova_add_fuzzer(fuzz_wallet_import wallet_import_fuzz.cpp)
target_link_libraries(fuzz_wallet_import PRIVATE nova_wallet nova_consensus)
