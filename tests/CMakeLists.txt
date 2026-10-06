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

add_executable(test_compare_scheduling
    test_compare_scheduling.c
    mock_platform.c
)

target_link_libraries(test_compare_scheduling PRIVATE pulse_generator unity)

add_test(NAME compare_scheduling COMMAND test_compare_scheduling)

add_executable(test_multi_instance
    test_multi_instance.c
    mock_platform.c
)

target_link_libraries(test_multi_instance PRIVATE pulse_generator unity)

add_test(NAME multi_instance COMMAND test_multi_instance)

add_executable(test_scheduled
    test_scheduled.c
    mock_platform.c
)

target_link_libraries(test_scheduled PRIVATE pulse_generator unity)

add_test(NAME scheduled COMMAND test_scheduled)
