# Xbox Input → XInput Compatibility Layer
#   input_map.c     the host -> Duke mapping table and pure helpers
#   input_core.c    public API, connection cache, probe backoff
#   xinput_device.c host backends (XInput on Windows, SDL2 elsewhere)

add_library(xbox_input STATIC
    input_map.c
    input_core.c
    xinput_device.c
)

target_include_directories(xbox_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    target_link_libraries(xbox_input PUBLIC platform xinput)
else()
    find_package(SDL2 REQUIRED)
    target_link_libraries(xbox_input PUBLIC platform SDL2::SDL2 m)
endif()

if(MSVC)
    target_compile_options(xbox_input PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_input PRIVATE -Wall -Wextra -O2)
endif()

