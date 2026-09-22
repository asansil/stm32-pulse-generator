include(FetchContent)

FetchContent_Declare(
    unity
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG v2.6.0
)
FetchContent_MakeAvailable(unity)

add_executable(test_smoke
    test_smoke.c
)

target_link_libraries(test_smoke PRIVATE pulse_generator unity)

add_test(NAME smoke COMMAND test_smoke)
