add_executable(novacoin-cli main.cpp)
nova_apply_project_options(novacoin-cli)
target_link_libraries(novacoin-cli PRIVATE nova_rpc nova_wallet)
