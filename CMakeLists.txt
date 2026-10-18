cmake_minimum_required(VERSION 3.21)
project(pulse_generator C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(pulse_generator
    src/pulse_generator.c
    src/pulse_generator_util.c
)

target_include_directories(pulse_generator PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pulse_generator PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

option(PULSE_GENERATOR_BUILD_TESTS "Build host unit tests for pulse_generator" OFF)

if(PULSE_GENERATOR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
