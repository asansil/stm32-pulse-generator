cmake_minimum_required(VERSION 3.15)
project(pulse_generator C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(pulse_generator
    src/pulse_generator.c
)

target_include_directories(pulse_generator PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

option(PULSEGEN_BUILD_TESTS "Build host unit tests for pulse_generator" OFF)

if(PULSEGEN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
