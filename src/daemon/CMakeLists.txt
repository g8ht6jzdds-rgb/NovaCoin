add_executable(novacoind main.cpp)
nova_apply_project_options(novacoind)
target_link_libraries(novacoind PRIVATE nova_node nova_chain nova_net nova_rpc nova_storage)
