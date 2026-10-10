# Xbox D3D8 Compatibility Layer
#   _WIN32 -> backed by Direct3D 11 (d3d8_*.c, d3d8_combiners.c, d3d8_vsh.c)
#   Both   -> d3d8_vsh_parse.c, d3d8_combiners_parse.c (NV2A vertex-program
#             and register-combiner decoders) and d3d8_vsh_hlsl.c,
#             d3d8_combiners_hlsl.c (their HLSL emitters), d3d8_vsh_msl.c,
#             d3d8_combiners_msl.c (their MSL emitters): no graphics API
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
        d3d8_vsh_hlsl.c
        d3d8_combiners_hlsl.c
        d3d8_vsh_msl.c
        d3d8_combiners_msl.c
        d3d8_vsh_glsl.c
        d3d8_combiners_glsl.c
        nv2a_pcache.c
        nv2a_pb_d3d11.c
    )
    target_link_libraries(xbox_d3d8 PUBLIC platform d3d11 dxgi dxguid d3dcompiler)
    # nv2a_pb_d3d11.c's window records keys in src/input's key table.
    target_link_libraries(xbox_d3d8 PUBLIC xbox_input)
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
    # The pushbuffer executor's D3D11 backend, driven by a synthetic stream.
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/d3d11_backend_smoke
                     ${CMAKE_CURRENT_BINARY_DIR}/d3d11_backend_smoke)
else()
    add_library(xbox_d3d8 STATIC
        d3d8_gl.c
        d3d8_vsh_parse.c
        d3d8_combiners_parse.c
        d3d8_vsh_hlsl.c
        d3d8_combiners_hlsl.c
        d3d8_vsh_msl.c
        d3d8_combiners_msl.c
        d3d8_vsh_glsl.c
        d3d8_combiners_glsl.c
        nv2a_pcache.c
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
    # The toolkit revision at configure time: the pipeline caches' version
    # key (nv2a_pcache.h), so a file from another build is dropped.
    execute_process(COMMAND git describe --always --dirty --abbrev=12
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                    OUTPUT_VARIABLE XR_GIT_REV OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if(NOT XR_GIT_REV)
        set(XR_GIT_REV "unknown")
    endif()
    if(XBOXRECOMP_HAVE_VULKAN)
        # The pushbuffer executor's Vulkan backend (RECOMP_PB_BACKEND=vulkan),
        # its device (shared with the window's swapchain) and glslang.
        target_sources(xbox_d3d8 PRIVATE nv2a_vk_core.c nv2a_vk_glsl.c nv2a_pb_vulkan.c)
        # For a title's own sources too (its default backend).
        target_compile_definitions(xbox_d3d8 PUBLIC XBOXRECOMP_HAVE_VULKAN=1)
        set_source_files_properties(nv2a_pb_vulkan.c PROPERTIES
            COMPILE_DEFINITIONS "XBOXRECOMP_GIT_REV=\"${XR_GIT_REV}\"")
        target_include_directories(xbox_d3d8 PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../kernel ${CMAKE_CURRENT_SOURCE_DIR}/..)
        target_link_libraries(xbox_d3d8 PUBLIC Vulkan::Vulkan glslang::glslang
                              glslang::glslang-default-resource-limits)
        # A static glslang (the Steam Runtime SDK's) is C++: its runtime
        # comes with it, as this C program's link would not add it.
        # The GLSL emitters' output through glslang (no device needed).
        add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/d3d8_glsl_split
                         ${CMAKE_CURRENT_BINARY_DIR}/d3d8_glsl_split)
        get_target_property(_xr_glslang_type glslang::glslang TYPE)
        if(_xr_glslang_type STREQUAL "STATIC_LIBRARY")
            # Its config names SPIRV-Tools-opt but not what that needs
            # (SPIRV-Tools), and GNU ld wants them after it, in order.
            target_link_libraries(xbox_d3d8 PUBLIC SPIRV-Tools-opt SPIRV-Tools stdc++ m pthread)
        endif()
    endif()
    if(APPLE)
        # The pushbuffer executor's Metal backend (RECOMP_PB_BACKEND=metal).
        # Manual retain/release (no -fobjc-arc); shaders are MSL compiled at
        # runtime, so no metallib build step.
        target_sources(xbox_d3d8 PRIVATE nv2a_pb_metal.m)
        # The pipeline cache's version key (nv2a_pcache.h): the toolkit
        # revision at configure time, so a file from another build is dropped.
        set_source_files_properties(nv2a_pb_metal.m PROPERTIES
            COMPILE_DEFINITIONS "XBOXRECOMP_GIT_REV=\"${XR_GIT_REV}\"")
        target_include_directories(xbox_d3d8 PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../kernel ${CMAKE_CURRENT_SOURCE_DIR}/..)
        target_link_libraries(xbox_d3d8 PUBLIC
            "-framework Metal" "-framework QuartzCore" "-framework Foundation")
    endif()
    # The executor's backend, driven by a synthetic stream: Metal or Vulkan
    # against the CPU rasteriser, frame by frame.
    if(APPLE OR XBOXRECOMP_HAVE_VULKAN)
        add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/nv2a_backend_smoke
                         ${CMAKE_CURRENT_BINARY_DIR}/nv2a_backend_smoke)
    endif()
endif()

target_include_directories(xbox_d3d8 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The CPU rasteriser's texture cache, driven by a synthetic stream (POSIX:
# the Proton test set does not run it).
if(NOT WIN32)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/pb_tex_cache
                     ${CMAKE_CURRENT_BINARY_DIR}/pb_tex_cache)
endif()

# The HLSL the parse -> generate pipeline emits, against recorded hashes
# (every host: the emitters are API-free).
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/d3d8_hlsl_split
                 ${CMAKE_CURRENT_BINARY_DIR}/d3d8_hlsl_split)

if(MSVC)
    target_compile_options(xbox_d3d8 PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_d3d8 PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()
# The MSL emitters' (d3d8_msl.h) hash test; it builds on every host, and on
# macOS also compiles each case with Metal.
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/d3d8_msl_split
                 ${CMAKE_CURRENT_BINARY_DIR}/d3d8_msl_split)
