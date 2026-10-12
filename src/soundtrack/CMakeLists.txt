# Custom soundtracks: the user's music folder as the dashboard's ST.DB, and
# its songs decoded by the host (xbox_soundtrack.h, docs/runtime/soundtrack.md).
#
# xst_library.c and xst_decode.c are plain C with no dependency on the
# runtime (tests/soundtrack builds just those two); xst_host.c is the part
# that knows the emulated disk. The decoders are single files under
# third_party/ (dr_libs, stb_vorbis: public domain or MIT), compiled into
# xst_decode.c.

add_library(xbox_soundtrack STATIC
    xst_library.c
    xst_decode.c
    xst_host.c
)
target_include_directories(xbox_soundtrack PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(xbox_soundtrack PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/../kernel
)
# platform carries the src/ include path and, off Windows, the Win32 names
# kernel.h is written in.
target_link_libraries(xbox_soundtrack PUBLIC platform)
if(NOT WIN32)
    target_link_libraries(xbox_soundtrack PRIVATE m)
endif()
if(MSVC)
    target_compile_options(xbox_soundtrack PRIVATE /W3 /O2)
    target_compile_definitions(xbox_soundtrack PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(xbox_soundtrack PRIVATE -Wall -Wextra -O2)
endif()
