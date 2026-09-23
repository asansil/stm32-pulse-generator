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
