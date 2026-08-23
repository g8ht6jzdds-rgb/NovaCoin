find_program(NOVA_CLANG_FORMAT_EXECUTABLE NAMES clang-format
    HINTS "${CMAKE_CURRENT_LIST_DIR}/../.toolchain/vs-buildtools/VC/Tools/Llvm/x64/bin"
    REQUIRED)

file(GLOB_RECURSE NOVA_FORMAT_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/../src/*.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/*.hpp"
    "${CMAKE_CURRENT_LIST_DIR}/../tests/*.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../tests/*.hpp"
    "${CMAKE_CURRENT_LIST_DIR}/../fuzz/*.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../fuzz/*.hpp")

execute_process(
    COMMAND ${NOVA_CLANG_FORMAT_EXECUTABLE} --dry-run --Werror --style=file ${NOVA_FORMAT_SOURCES}
    RESULT_VARIABLE NOVA_FORMAT_RESULT)

if(NOT NOVA_FORMAT_RESULT EQUAL 0)
    message(FATAL_ERROR "clang-format check failed")
endif()
