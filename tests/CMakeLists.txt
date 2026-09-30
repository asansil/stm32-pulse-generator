include(FetchContent)

FetchContent_Declare(
    unity
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG v2.6.0
)
FetchContent_MakeAvailable(unity)

add_executable(test_init
    test_init.c
    mock_platform.c
)

target_link_libraries(test_init PRIVATE pulse_generator unity)

add_test(NAME init COMMAND test_init)

add_executable(test_fixed_count
    test_fixed_count.c
    mock_platform.c
)

target_link_libraries(test_fixed_count PRIVATE pulse_generator unity)

add_test(NAME fixed_count COMMAND test_fixed_count)

add_executable(test_continuous
    test_continuous.c
    mock_platform.c
)

target_link_libraries(test_continuous PRIVATE pulse_generator unity)

add_test(NAME continuous COMMAND test_continuous)
