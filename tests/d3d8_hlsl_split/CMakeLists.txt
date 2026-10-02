# d3d8_hlsl_split - the HLSL text d3d8_vsh.c and d3d8_combiners.c emit, for
# a fixed set of pseudo-random programs and combiner registers, hashed and
# checked against the value recorded before the parsers were split out
# (d3d8_vsh_parse.c, d3d8_combiners_parse.c). No D3D11 device needed.
#   cmake --build build-win --target d3d8_hlsl_split
# Run under Wine/Proton (or ctest); HLSL_DUMP=<file> writes the text for a
# diff, HLSL_EXPECT_OUT=<file> a new expected.h.

if(WIN32)
    add_executable(d3d8_hlsl_split hlsl_split.c)
    target_link_libraries(d3d8_hlsl_split PRIVATE xbox_d3d8)
    # Cross-built: ctest runs it through CMAKE_CROSSCOMPILING_EMULATOR
    # (a Wine/Proton wrapper) when one is configured.
    enable_testing()
    add_test(NAME d3d8_hlsl_split COMMAND d3d8_hlsl_split)
endif()
