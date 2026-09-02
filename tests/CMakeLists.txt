find_package(GTest 1.11 CONFIG REQUIRED)

add_executable(nova_unit_tests
    empty_suite.cpp
    serialization_tests.cpp
    transaction_tests.cpp
    utxo_tests.cpp
    block_tests.cpp
    monetary_tests.cpp
    pow_tests.cpp
    validation_tests.cpp
    chainstate_tests.cpp
    mempool_tests.cpp
    p2p_tests.cpp
    wallet_tests.cpp
    address_tests.cpp
    rpc_tests.cpp
    network_selection_tests.cpp
    network_params_tests.cpp
    regtest_harness_tests.cpp
    storage_tests.cpp
    network_identity_tests.cpp
    bootstrap_tests.cpp
    explorer_tests.cpp
    faucet_tests.cpp
    property_tests.cpp)
nova_apply_project_options(nova_unit_tests)
target_link_libraries(nova_unit_tests PRIVATE
    GTest::gtest_main
    nova_crypto
    nova_primitives
    nova_consensus
    nova_chain
    nova_net
    nova_wallet
    nova_rpc
    nova_node
    nova_explorer)
target_link_libraries(nova_unit_tests PRIVATE nova_faucet)

add_test(NAME nova.unit COMMAND nova_unit_tests)
set_tests_properties(nova.unit PROPERTIES LABELS "unit;crypto;primitives;chain")

if(UNIX)
    find_program(NOVA_BASH_EXECUTABLE bash REQUIRED)
    add_test(NAME nova.regtest_rpc_process
        COMMAND ${NOVA_BASH_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/regtest_rpc_harness.sh
                $<TARGET_FILE:novacoind> $<TARGET_FILE:nova-explorer>)
    set_tests_properties(nova.regtest_rpc_process PROPERTIES
        LABELS "integration;regtest;rpc;explorer" TIMEOUT 90
        ENVIRONMENT "NOVACOIN_REGTEST_ARTIFACT_DIR=${CMAKE_BINARY_DIR}/regtest-artifacts")
endif()
