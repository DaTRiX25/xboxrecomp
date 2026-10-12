# restart - recomp_restart.h: what a restart starts (the arguments,
# directory and environment as given), and a real one.
#   cmake -S tests/restart -B build/restart && cmake --build build/restart
#   ctest --test-dir build/restart
cmake_minimum_required(VERSION 3.20)
project(restart_test C)
set(CMAKE_C_STANDARD 11)
add_executable(restart_test test_main.c ../../src/platform/recomp_restart.c)
target_include_directories(restart_test PRIVATE ../../src/platform)
enable_testing()
add_test(NAME restart_snapshot COMMAND restart_test snapshot "a b" "ő")
# The program restarts itself once: the second life sees the same arguments,
# the same directory, the environment as it was given (not what the first
# life put into it), the same process id off Windows, and a mark the first
# life left in a file.
add_test(NAME restart_real
    COMMAND ${CMAKE_COMMAND} -E env RESTART_TEST_GIVEN=given
            $<TARGET_FILE:restart_test> real ${CMAKE_CURRENT_BINARY_DIR}/restart_mark.txt "an argument")
