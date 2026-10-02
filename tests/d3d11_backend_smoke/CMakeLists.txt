# d3d11_backend_smoke - the pushbuffer executor drawing through D3D11
#
# Feeds nv2a_pb_exec_method() a hand-written method stream (surface, clear,
# one quad, flip) with RECOMP_PB_BACKEND=d3d11, so the backend in
# src/d3d/nv2a_pb_d3d11.c can be checked under Proton without a title.
#   cmake --build build-win --target d3d11_backend_smoke

if(WIN32)
    add_executable(d3d11_backend_smoke smoke.c)
    target_link_libraries(d3d11_backend_smoke PRIVATE xbox_kernel xbox_d3d8 xbox_video
                          d3d11 dxgi dxguid)
    target_include_directories(d3d11_backend_smoke PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../../src/d3d)
endif()
