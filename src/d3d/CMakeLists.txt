# Xbox D3D8 Compatibility Layer
#   _WIN32 -> backed by Direct3D 11 (d3d8_*.c, d3d8_combiners.c, d3d8_vsh.c)
#   Both   -> d3d8_vsh_parse.c, d3d8_combiners_parse.c (NV2A vertex-program
#             and register-combiner decoders, no graphics API)
#   POSIX  -> backed by OpenGL 3.3 core via SDL2 + libepoxy (d3d8_gl.c)

if(WIN32)
    add_library(xbox_d3d8 STATIC
        d3d8_device.c
        d3d8_gamma.c
        d3d8_resources.c
        d3d8_shaders.c
        d3d8_states.c
        d3d8_combiners.c
        d3d8_vsh.c
        d3d8_vsh_parse.c
        d3d8_combiners_parse.c
        nv2a_pb_d3d11.c
    )
    target_link_libraries(xbox_d3d8 PUBLIC platform d3d11 dxgi dxguid d3dcompiler)
    # nv2a_pb_d3d11.c: the pushbuffer executor's state (nv2a_pb_state.h) and
    # guest memory (xbox_memory_layout.h) live with the kernel.
    target_include_directories(xbox_d3d8 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../kernel ${CMAKE_CURRENT_SOURCE_DIR}/..)

    # Offline smoke test for the pure format layer (no D3D11 device needed).
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/d3d8_smoke
                     ${CMAKE_CURRENT_BINARY_DIR}/d3d8_smoke)
    # D3DCompile under the host's d3dcompiler_47 (Wine's, under Proton).
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/d3dcompile_smoke
                     ${CMAKE_CURRENT_BINARY_DIR}/d3dcompile_smoke)
    # The HLSL the parse -> generate pipeline emits, against a recorded hash.
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/d3d8_hlsl_split
                     ${CMAKE_CURRENT_BINARY_DIR}/d3d8_hlsl_split)
    # The pushbuffer executor's D3D11 backend, driven by a synthetic stream.
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/d3d11_backend_smoke
                     ${CMAKE_CURRENT_BINARY_DIR}/d3d11_backend_smoke)
else()
    add_library(xbox_d3d8 STATIC
        d3d8_gl.c
        d3d8_vsh_parse.c
        d3d8_combiners_parse.c
    )
    find_package(SDL2 REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EPOXY REQUIRED epoxy)
    target_include_directories(xbox_d3d8 PRIVATE ${EPOXY_INCLUDE_DIRS})
    # EPOXY_LINK_LIBRARIES, not EPOXY_LIBRARIES: the latter is the bare name
    # `epoxy`, which only resolves if EPOXY_LIBRARY_DIRS happens to be on the
    # link path. That holds for the top-level build and not when this is pulled
    # in via add_subdirectory from tests/, where the link fails with
    # "library 'epoxy' not found". The _LINK_LIBRARIES form is absolute paths.
    target_link_libraries(xbox_d3d8 PUBLIC platform SDL2::SDL2 ${EPOXY_LINK_LIBRARIES})
endif()

target_include_directories(xbox_d3d8 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(xbox_d3d8 PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_d3d8 PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()
