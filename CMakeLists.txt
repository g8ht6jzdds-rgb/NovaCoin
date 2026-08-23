cmake_minimum_required(VERSION 3.25)

project(NovaCoin VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(NOVA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(NOVA_ENABLE_SANITIZERS "Enable one or more runtime sanitizers" OFF)
option(NOVA_ENABLE_ADDRESS_SANITIZER "Enable AddressSanitizer" OFF)
option(NOVA_ENABLE_UNDEFINED_BEHAVIOR_SANITIZER "Enable UndefinedBehaviorSanitizer" OFF)
option(NOVA_ENABLE_CLANG_TIDY "Run clang-tidy while compiling project targets" OFF)
option(NOVA_BUILD_FUZZERS "Build fuzzing targets when they are added" OFF)
option(NOVA_SANITIZER_STATIC_RUNTIME
    "Use the static MSVC runtime required by libFuzzer on Windows" OFF)

if((NOVA_ENABLE_ADDRESS_SANITIZER OR NOVA_ENABLE_UNDEFINED_BEHAVIOR_SANITIZER)
   AND NOT NOVA_ENABLE_SANITIZERS)
    message(FATAL_ERROR
        "Set NOVA_ENABLE_SANITIZERS=ON before selecting an individual sanitizer.")
endif()

if(NOVA_ENABLE_SANITIZERS
   AND NOT (NOVA_ENABLE_ADDRESS_SANITIZER OR NOVA_ENABLE_UNDEFINED_BEHAVIOR_SANITIZER))
    message(FATAL_ERROR
        "Select AddressSanitizer and/or UndefinedBehaviorSanitizer when sanitizers are enabled.")
endif()

# Clang's Windows ASan runtime is incompatible with the MSVC debug CRT. Use
# the release dynamic CRT for test binaries. libFuzzer instead requires the
# static release CRT, so fuzzers are built in their own configuration with
# NOVA_SANITIZER_STATIC_RUNTIME=ON. This affects instrumentation only.
if(NOVA_ENABLE_SANITIZERS AND WIN32)
    if(NOVA_SANITIZER_STATIC_RUNTIME)
        set(nova_windows_sanitizer_runtime "MultiThreaded")
    else()
        set(nova_windows_sanitizer_runtime "MultiThreadedDLL")
    endif()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "${nova_windows_sanitizer_runtime}" CACHE STRING
        "MSVC runtime library used by NovaCoin targets" FORCE)
endif()

add_library(nova_project_options INTERFACE)

if(MSVC)
    target_compile_options(nova_project_options INTERFACE /W4 /permissive- /EHsc)
    if(NOVA_WARNINGS_AS_ERRORS)
        target_compile_options(nova_project_options INTERFACE /WX)
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(nova_project_options INTERFACE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    if(NOVA_WARNINGS_AS_ERRORS)
        target_compile_options(nova_project_options INTERFACE -Werror)
    endif()
else()
    message(WARNING "NovaCoin strict warning flags are not defined for this compiler.")
endif()

if(NOVA_ENABLE_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "Sanitizer builds currently require Clang or GCC.")
    endif()

    set(nova_sanitizer_flags "")
    if(NOVA_ENABLE_ADDRESS_SANITIZER)
        list(APPEND nova_sanitizer_flags -fsanitize=address)
    endif()
    if(NOVA_ENABLE_UNDEFINED_BEHAVIOR_SANITIZER)
        list(APPEND nova_sanitizer_flags -fsanitize=undefined)
    endif()
    target_compile_options(nova_project_options INTERFACE ${nova_sanitizer_flags})
    target_link_options(nova_project_options INTERFACE ${nova_sanitizer_flags})

    if(WIN32 AND NOVA_ENABLE_ADDRESS_SANITIZER)
        execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" -print-resource-dir
            OUTPUT_VARIABLE nova_clang_resource_dir
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE nova_resource_dir_result)
        if(NOT nova_resource_dir_result EQUAL 0 OR nova_clang_resource_dir STREQUAL "")
            message(FATAL_ERROR "Unable to locate the Clang resource directory for AddressSanitizer.")
        endif()

        set(NOVA_ASAN_RUNTIME_DLL
            "${nova_clang_resource_dir}/lib/windows/clang_rt.asan_dynamic-x86_64.dll")
        if(NOT EXISTS "${NOVA_ASAN_RUNTIME_DLL}")
            message(FATAL_ERROR "AddressSanitizer runtime is missing: ${NOVA_ASAN_RUNTIME_DLL}")
        endif()
    endif()
endif()

if(NOVA_ENABLE_CLANG_TIDY)
    find_program(NOVA_CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
    set(CMAKE_CXX_CLANG_TIDY "${NOVA_CLANG_TIDY_EXECUTABLE};--config-file=${CMAKE_SOURCE_DIR}/.clang-tidy")
endif()

function(nova_apply_project_options target)
    target_link_libraries(${target} PRIVATE nova_project_options)

    # CTest does not inherit a developer's LLVM runtime PATH.  Stage the
    # version-pinned ASan DLL beside each instrumented executable instead.
    if(DEFINED NOVA_ASAN_RUNTIME_DLL)
        get_target_property(nova_target_type ${target} TYPE)
        if(nova_target_type STREQUAL "EXECUTABLE")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${NOVA_ASAN_RUNTIME_DLL}" "$<TARGET_FILE_DIR:${target}>"
                VERBATIM)
        endif()
    endif()
endfunction()

add_subdirectory(src)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(NOVA_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()

add_custom_target(format-check
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_SOURCE_DIR}/scripts/check-format.cmake
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Checking C++ formatting with clang-format")
