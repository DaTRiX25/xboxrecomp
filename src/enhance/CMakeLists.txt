# Enhancements layer (opt-in: XBOXRECOMP_ENHANCE=ON in the root CMakeLists).
#   recomp_cfg.c   TOML-subset reader, no dependencies (C11 + libc)
#   enhance_cfg.c  enhance.toml under the environment: env > title file >
#                  root file > default
# docs/runtime/enhance-config.md has the file format and the keys.

add_library(xbox_enhance STATIC recomp_cfg.c enhance_cfg.c)
target_include_directories(xbox_enhance PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xbox_enhance PUBLIC recomp_env)
if(NOT MSVC)
    target_compile_options(xbox_enhance PRIVATE -Wall -Wextra)
endif()
