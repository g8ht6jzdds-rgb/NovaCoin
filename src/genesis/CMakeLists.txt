add_executable(nova-genesis main.cpp)
nova_apply_project_options(nova-genesis)
target_link_libraries(nova-genesis PRIVATE nova_consensus nova_primitives)
