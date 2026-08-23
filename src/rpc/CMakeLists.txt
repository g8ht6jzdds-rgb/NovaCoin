add_library(nova_rpc STATIC rpc.cpp http_server.cpp)
nova_apply_project_options(nova_rpc)
target_link_libraries(nova_rpc PUBLIC nova_chain nova_net nova_wallet)
