find_package(GTest 1.11 CONFIG REQUIRED)

add_executable(nova_unit_tests
    version_tests.cpp
    readiness_tests.cpp
    empty_suite.cpp
    serialization_tests.cpp
    transaction_tests.cpp
    utxo_tests.cpp
    block_tests.cpp
    monetary_tests.cpp
    pow_tests.cpp
    validation_tests.cpp
    chainstate_tests.cpp
    network_chainstate_tests.cpp
    inspection_tests.cpp
    mempool_tests.cpp
    p2p_tests.cpp
    peer_discovery_tests.cpp
    body_request_recovery_tests.cpp
    sync_tests.cpp
    wallet_tests.cpp
    address_tests.cpp
    rpc_tests.cpp
    network_selection_tests.cpp
    network_params_tests.cpp
    regtest_harness_tests.cpp
    storage_tests.cpp
    generation_store_tests.cpp
    network_identity_tests.cpp
    bootstrap_tests.cpp
    explorer_tests.cpp
    faucet_tests.cpp
    property_tests.cpp)
nova_apply_project_options(nova_unit_tests)
target_link_libraries(nova_unit_tests PRIVATE nova_version)

find_package(Python3 3.8 COMPONENTS Interpreter REQUIRED)
add_executable(nova_generation_crash_worker generation_crash_worker.cpp)
nova_apply_project_options(nova_generation_crash_worker)
target_link_libraries(nova_generation_crash_worker PRIVATE nova_storage)
add_test(NAME nova.generation_crash
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/scripts/test_generation_crash.py
        $<TARGET_FILE:nova_generation_crash_worker>)
set_tests_properties(nova.generation_crash PROPERTIES TIMEOUT 180 LABELS "unit;storage;recovery")
add_test(NAME nova.release_policy
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/scripts/test_release.py)
set_tests_properties(nova.release_policy PROPERTIES TIMEOUT 60 LABELS "unit;release;policy")
add_test(NAME nova.reorg_rpc_deadlines
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/scripts/test_reorg_rpc_deadlines.py)
set_tests_properties(nova.reorg_rpc_deadlines PROPERTIES TIMEOUT 5 LABELS "unit;regtest;evidence")
add_test(NAME nova.build_metadata
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/scripts/test_build_metadata.py
        $<TARGET_FILE:nova-build-info> $<TARGET_FILE:novacoind> $<TARGET_FILE:novacoin-cli>
        ${CMAKE_SOURCE_DIR})
set_tests_properties(nova.build_metadata PROPERTIES TIMEOUT 30 LABELS "unit;release;version")
add_test(NAME nova.mainnet_lock
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/scripts/test_mainnet_lock.py
        $<TARGET_FILE:novacoind>)
set_tests_properties(nova.mainnet_lock PROPERTIES TIMEOUT 30 LABELS "unit;readiness;mainnet-lock")
add_test(NAME nova.readiness_policy
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/scripts/test_readiness_policy.py)
set_tests_properties(nova.readiness_policy PROPERTIES TIMEOUT 20 LABELS "unit;readiness;policy")
add_test(NAME nova.consensus_evidence
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/scripts/test_consensus_evidence.py)
set_tests_properties(nova.consensus_evidence PROPERTIES TIMEOUT 20 LABELS "unit;readiness;evidence")
add_test(NAME nova.executable_version
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/scripts/test_version.py
        --cmake ${CMAKE_COMMAND} --source ${CMAKE_SOURCE_DIR}
        --header ${PROJECT_BINARY_DIR}/generated/build/version_data.hpp
        $<TARGET_FILE:novacoind> $<TARGET_FILE:novacoin-cli>
        $<TARGET_FILE:nova-genesis> $<TARGET_FILE:nova-explorer>
        $<TARGET_FILE:novacoin-faucet>)
set_tests_properties(nova.executable_version PROPERTIES TIMEOUT 90 LABELS "unit;version")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT NOVA_ENABLE_SANITIZERS)
    add_library(nova_version_guard SHARED version_guard.cpp)
    nova_apply_project_options(nova_version_guard)
    add_executable(nova_version_guard_probe version_guard_probe.cpp)
    nova_apply_project_options(nova_version_guard_probe)
    add_test(NAME nova.version_side_effect_guard
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/scripts/test_version_guard.py
            $<TARGET_FILE:nova_version_guard> $<TARGET_FILE:nova_version_guard_probe>
            $<TARGET_FILE:novacoind> $<TARGET_FILE:novacoin-cli>
            $<TARGET_FILE:nova-genesis> $<TARGET_FILE:nova-explorer>
            $<TARGET_FILE:novacoin-faucet>)
    set_tests_properties(nova.version_side_effect_guard PROPERTIES TIMEOUT 30 LABELS "unit;version;linux")
endif()
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
target_link_libraries(nova_unit_tests PRIVATE nova_inspection)

# The diagnostic corpus has no storage/node dependency. Keep its focused tests
# independently buildable so evidence generation does not depend on recovery
# or service test fixtures.
add_executable(nova_freeze_corpus_tests freeze_corpus_tests.cpp)
nova_apply_project_options(nova_freeze_corpus_tests)
target_link_libraries(nova_freeze_corpus_tests PRIVATE GTest::gtest_main nova_freeze_corpus)
add_test(NAME nova.freeze_corpus COMMAND nova_freeze_corpus_tests)
set_tests_properties(nova.freeze_corpus PROPERTIES TIMEOUT 30 LABELS "unit;readiness;determinism")

add_executable(nova_readiness_vectors readiness_vectors.cpp)
nova_apply_project_options(nova_readiness_vectors)
target_link_libraries(nova_readiness_vectors PRIVATE nova_chain)
add_test(NAME nova.readiness_vectors COMMAND nova_readiness_vectors)
set_tests_properties(nova.readiness_vectors PROPERTIES TIMEOUT 60 LABELS "unit;readiness;determinism")

add_executable(nova_protocol_conformance_tests protocol_conformance_tests.cpp)
nova_apply_project_options(nova_protocol_conformance_tests)
target_link_libraries(nova_protocol_conformance_tests PRIVATE GTest::gtest_main nova_chain)
target_compile_definitions(nova_protocol_conformance_tests PRIVATE
    NOVA_PROTOCOL_VECTOR_DIR="${CMAKE_SOURCE_DIR}/test/data/protocol")
add_test(NAME nova.protocol_conformance COMMAND nova_protocol_conformance_tests)
set_tests_properties(nova.protocol_conformance PROPERTIES TIMEOUT 60 LABELS "unit;consensus;protocol")
add_test(NAME nova.freeze_corpus_cli
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/scripts/test_freeze_corpus.py
        $<TARGET_FILE:nova-freeze-corpus>)
set_tests_properties(nova.freeze_corpus_cli PROPERTIES TIMEOUT 30 LABELS "unit;readiness;determinism")

add_test(NAME nova.unit COMMAND nova_unit_tests)
set_tests_properties(nova.unit PROPERTIES LABELS "unit;crypto;primitives;chain")

# Named release gates reuse real assertions instead of accepting hand-written PASS records.
add_test(NAME nova.consensus COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_SOURCE_DIR}/scripts/run_test_group.py $<TARGET_FILE:nova_unit_tests>
    --gtest_filter=Crypto*:Serialization*:Transaction*:UTXO*:Block*:Monetary*:Pow*:Consensus*:Mainnet*:NetworkInitialization*:NetworkParams*:GenesisGenerator*:ChainState*)
add_test(NAME nova.network_integration COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_SOURCE_DIR}/scripts/run_test_group.py $<TARGET_FILE:nova_unit_tests>
    --gtest_filter=P2P*:PeerManager*:TcpTransport*:SyncProgress*:RegtestHarness.ExercisesFourNodeLifecycleAndAdversarialPaths)
add_test(NAME nova.wallet_integration COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_SOURCE_DIR}/scripts/run_test_group.py $<TARGET_FILE:nova_unit_tests>
    --gtest_filter=Wallet*:RpcIntegration*:RegtestHarness.ExercisesFourNodeLifecycleAndAdversarialPaths)
add_test(NAME nova.reorg COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_SOURCE_DIR}/scripts/run_test_group.py $<TARGET_FILE:nova_unit_tests>
    --gtest_filter=ChainStateSelection.*:NetworkInitialization.SyntheticSignedSpendRollbackReorgAndRestartReplay:MainnetReadiness.Reorganizations*)
set_tests_properties(nova.consensus nova.network_integration nova.wallet_integration nova.reorg
    PROPERTIES TIMEOUT 120 LABELS "release;regtest")

add_test(NAME nova.testnet_release_record
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_SOURCE_DIR}/scripts/verify_testnet_release_record.cmake)
set_tests_properties(nova.testnet_release_record PROPERTIES LABELS "unit;testnet;release-record")

if(UNIX)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(nova_inspection_fixture inspection_fixture.cpp)
        nova_apply_project_options(nova_inspection_fixture)
        target_link_libraries(nova_inspection_fixture PRIVATE nova_inspection)
        find_package(Python3 3.8 COMPONENTS Interpreter REQUIRED)
        add_test(NAME nova.offline_inspection_boundary
            COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/scripts/test_offline_inspection.py
                    $<TARGET_FILE:nova-chain-inspect> $<TARGET_FILE:nova_inspection_fixture>)
        set_tests_properties(nova.offline_inspection_boundary PROPERTIES TIMEOUT 90 LABELS "unit;inspection;linux")
    endif()
    add_executable(nova_pending_header_loopback_tests pending_header_loopback_tests.cpp)
    nova_apply_project_options(nova_pending_header_loopback_tests)
    target_link_libraries(nova_pending_header_loopback_tests PRIVATE GTest::gtest_main nova_node)
    add_test(NAME nova.pending_header_loopback COMMAND nova_pending_header_loopback_tests)
    set_tests_properties(nova.pending_header_loopback PROPERTIES
        LABELS "integration;regtest;p2p;headers" TIMEOUT 50)
    find_package(Python3 3.8 COMPONENTS Interpreter REQUIRED)
    add_test(NAME nova.reorg_evidence_verifier
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/scripts/test_reorg_evidence.py)
    set_tests_properties(nova.reorg_evidence_verifier PROPERTIES
        LABELS "unit;regtest;evidence" TIMEOUT 5)
    add_test(NAME nova.regtest_peer_discovery
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/scripts/test_peer_discovery.py
                $<TARGET_FILE:novacoind> ${CMAKE_BINARY_DIR}/regtest-artifacts)
    set_tests_properties(nova.regtest_peer_discovery PROPERTIES
        LABELS "integration;regtest;discovery" TIMEOUT 180)
    add_test(NAME nova.regtest_static_bootstrap
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/test_static_bootstrap.py
                $<TARGET_FILE:novacoind> ${CMAKE_BINARY_DIR}/regtest-artifacts)
    set_tests_properties(nova.regtest_static_bootstrap PROPERTIES
        LABELS "integration;regtest;bootstrap" TIMEOUT 180)
    add_executable(nova_regtest_header_phase_peer regtest_header_phase_peer.cpp)
    nova_apply_project_options(nova_regtest_header_phase_peer)
    target_link_libraries(nova_regtest_header_phase_peer PRIVATE nova_net nova_consensus)
    add_test(NAME nova.header_phase_peer_fixture COMMAND nova_regtest_header_phase_peer --self-test)
    add_test(NAME nova.header_phase_peer_arguments
        COMMAND nova_regtest_header_phase_peer 65536 29638 invalid-hash missing-directory)
    set_tests_properties(nova.header_phase_peer_arguments PROPERTIES WILL_FAIL TRUE)
    set_tests_properties(nova.header_phase_peer_fixture nova.header_phase_peer_arguments
        PROPERTIES TIMEOUT 5 LABELS "unit;regtest;p2p;headers")
    add_executable(nova_regtest_invalid_transaction_peer regtest_invalid_transaction_peer.cpp)
    nova_apply_project_options(nova_regtest_invalid_transaction_peer)
    target_link_libraries(nova_regtest_invalid_transaction_peer PRIVATE nova_net nova_chain)
    add_test(NAME nova.invalid_transaction_peer_fixture
        COMMAND nova_regtest_invalid_transaction_peer --self-test)
    add_test(NAME nova.invalid_transaction_peer_arguments
        COMMAND nova_regtest_invalid_transaction_peer submit 65536 not-a-hash missing-file)
    set_tests_properties(nova.invalid_transaction_peer_arguments PROPERTIES WILL_FAIL TRUE)
    set_tests_properties(nova.invalid_transaction_peer_fixture nova.invalid_transaction_peer_arguments
        PROPERTIES TIMEOUT 5 LABELS "unit;regtest;p2p;transaction")
    add_executable(nova_regtest_invalid_block_peer regtest_invalid_block_peer.cpp)
    nova_apply_project_options(nova_regtest_invalid_block_peer)
    target_link_libraries(nova_regtest_invalid_block_peer PRIVATE nova_net nova_consensus)
    add_test(NAME nova.invalid_block_peer_fixture
        COMMAND nova_regtest_invalid_block_peer --self-test)
    add_test(NAME nova.invalid_block_peer_arguments
        COMMAND nova_regtest_invalid_block_peer 65536 not-a-hash)
    set_tests_properties(nova.invalid_block_peer_arguments PROPERTIES WILL_FAIL TRUE)
    set_tests_properties(nova.invalid_block_peer_fixture nova.invalid_block_peer_arguments
        PROPERTIES TIMEOUT 5 LABELS "unit;regtest;p2p")

    find_program(NOVA_BASH_EXECUTABLE bash REQUIRED)
    add_test(NAME nova.regtest_staging_collector
        COMMAND ${NOVA_BASH_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/test_collect_regtest_staging_evidence.sh
                ${CMAKE_SOURCE_DIR}/scripts/collect_regtest_staging_evidence.sh)
    set_tests_properties(nova.regtest_staging_collector PROPERTIES
        LABELS "unit;staging;operations" TIMEOUT 30)

    add_test(NAME nova.regtest_rpc_process
        COMMAND ${NOVA_BASH_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/regtest_rpc_harness.sh
                $<TARGET_FILE:novacoind> $<TARGET_FILE:nova-explorer>
                $<TARGET_FILE:nova_regtest_invalid_block_peer>
                $<TARGET_FILE:nova_regtest_invalid_transaction_peer>
                $<TARGET_FILE:nova_regtest_header_phase_peer>)
    set_tests_properties(nova.regtest_rpc_process PROPERTIES
        LABELS "integration;regtest;rpc;explorer" TIMEOUT 90
        ENVIRONMENT "NOVACOIN_REGTEST_ARTIFACT_DIR=${CMAKE_BINARY_DIR}/regtest-artifacts")
endif()
