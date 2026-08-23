add_library(nova_primitives STATIC serialization.cpp transaction.cpp block.cpp)
nova_apply_project_options(nova_primitives)
target_include_directories(nova_primitives PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(nova_primitives PUBLIC nova_crypto)
