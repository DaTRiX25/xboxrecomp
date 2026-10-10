# The in-game settings overlay (XBOXRECOMP_OVERLAY, root CMakeLists):
#   xbox_settings.c        the settings, settings.toml, the precedence (no ImGui)
#   overlay_ui.cpp    hotkeys, pause, input routing, the pages, the graph
#   overlay_metal.mm  the Metal renderer glue (macOS)
#   overlay_vulkan.cpp the Vulkan renderer glue
# Dear ImGui is vendored in third_party/imgui (see VENDORED.txt): the core and
# the SDL2, Metal and Vulkan backends, unmodified. A D3D11 backend is
# imgui_impl_dx11.cpp plus one glue file like the two here (the window stays
# SDL's).
find_package(SDL2 REQUIRED)

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/imgui)

add_library(xbox_overlay STATIC
    xbox_settings.c
    overlay_ui.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
)
set_property(TARGET xbox_overlay PROPERTY CXX_STANDARD 17)
target_include_directories(xbox_overlay PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(xbox_overlay PRIVATE ${IMGUI_DIR})
# Seen by the presenters and the host that call into it.
target_compile_definitions(xbox_overlay PUBLIC XBOXRECOMP_HAVE_OVERLAY=1)
target_link_libraries(xbox_overlay PUBLIC SDL2::SDL2 recomp_env xbox_enhance xbox_input xbox_kernel xbox_apu)

if(APPLE)
    target_sources(xbox_overlay PRIVATE
        overlay_metal.mm
        ${IMGUI_DIR}/backends/imgui_impl_metal.mm)
    # imgui_impl_metal.mm needs ARC; the glue is written for it too.
    set_source_files_properties(${IMGUI_DIR}/backends/imgui_impl_metal.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(xbox_overlay PUBLIC "-framework Metal" "-framework QuartzCore"
        "-framework Foundation" "-framework AppKit")
endif()

if(XBOXRECOMP_HAVE_VULKAN)
    target_sources(xbox_overlay PRIVATE
        overlay_vulkan.cpp
        ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)
    target_link_libraries(xbox_overlay PUBLIC Vulkan::Vulkan)
endif()

if(NOT MSVC)
    # The vendored files are built as they are; the glue is held to the
    # runtime's own warnings.
    set_source_files_properties(xbox_settings.c overlay_ui.cpp overlay_vulkan.cpp
        PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")
    if(APPLE)
        set_source_files_properties(overlay_metal.mm
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-Wall;-Wextra")
    endif()
endif()

# settings.toml: the file, the precedence, the power mode's two knobs.
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/overlay_settings
                 ${CMAKE_CURRENT_BINARY_DIR}/overlay_settings)

# The overlay off screen, with synthetic events (macOS): ninja overlay_shot.
if(APPLE)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/overlay_shot
                     ${CMAKE_CURRENT_BINARY_DIR}/overlay_shot)
endif()
