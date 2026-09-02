add_library(nova_faucet STATIC
    faucet.cpp
    http.cpp
    http_server.cpp)
nova_apply_project_options(nova_faucet)
target_link_libraries(nova_faucet PUBLIC nova_crypto nova_consensus nova_wallet)

add_executable(novacoin-faucet main.cpp)
nova_apply_project_options(novacoin-faucet)
target_link_libraries(novacoin-faucet PRIVATE nova_faucet)
