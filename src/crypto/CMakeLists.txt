find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
find_path(NOVA_SECP256K1_INCLUDE_DIR NAMES secp256k1.h REQUIRED)
find_library(NOVA_SECP256K1_LIBRARY NAMES secp256k1 REQUIRED)

add_library(nova_secp256k1 UNKNOWN IMPORTED)
set_target_properties(nova_secp256k1 PROPERTIES
    IMPORTED_LOCATION "${NOVA_SECP256K1_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${NOVA_SECP256K1_INCLUDE_DIR}")

add_library(nova_crypto STATIC hash.cpp keys.cpp)
nova_apply_project_options(nova_crypto)
target_include_directories(nova_crypto PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(nova_crypto PUBLIC OpenSSL::Crypto nova_secp256k1)
target_compile_definitions(nova_crypto PRIVATE SECP256K1_STATIC)
