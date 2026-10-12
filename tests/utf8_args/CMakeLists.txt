# utf8_args - a program's arguments from UTF-16 to UTF-8 (recomp_utf8_args.h).
# The conversion is plain C and runs on every host; on Windows the test also
# reads its own wide command line through recomp_win_utf8_args.
#   cmake -S tests/utf8_args -B build/utf8_args && cmake --build build/utf8_args
#   ctest --test-dir build/utf8_args
cmake_minimum_required(VERSION 3.20)
project(utf8_args_test C)
set(CMAKE_C_STANDARD 11)
add_executable(utf8_args_test test_main.c ../../src/platform/recomp_utf8_args.c)
target_include_directories(utf8_args_test PRIVATE ../../src/platform)
if(WIN32)
    target_link_libraries(utf8_args_test PRIVATE shell32)
endif()
enable_testing()
add_test(NAME utf8_args COMMAND utf8_args_test)
# Windows: the same letters through a real command line. They arrive right
# only in a program whose code page is UTF-8 (a manifest), which this test
# program is not: it reports what it got and passes either way when the code
# page is not UTF-8, and must match when it is.
if(WIN32)
    add_test(NAME utf8_args_command_line COMMAND utf8_args_test --echo "Zenék őű музыка")
endif()
