add_library(nova_node STATIC sync.cpp regtest_node.cpp peer_service.cpp body_request_recovery.cpp peer_discovery.cpp network_selection.cpp bootstrap_config.cpp)
nova_apply_project_options(nova_node)
target_link_libraries(nova_node PUBLIC nova_chain nova_net nova_storage nova_wallet)
