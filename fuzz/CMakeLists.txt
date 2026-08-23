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

add_custom_target(nova_fuzzers DEPENDS
    fuzz_deserializer
    fuzz_transaction_deserializer
    fuzz_block_deserializer
    fuzz_network_message
    fuzz_script_signature
    fuzz_address_parser
    fuzz_compact_target)
