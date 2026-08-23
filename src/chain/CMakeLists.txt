add_library(nova_chain STATIC utxo.cpp validation.cpp chainstate.cpp mempool.cpp)
nova_apply_project_options(nova_chain)
target_link_libraries(nova_chain PUBLIC nova_consensus nova_primitives)
