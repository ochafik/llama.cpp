# vlib-video: C++ video / Conv3D consumption layer
#
# Provides:
#   - vlib_tool_parser     : parse Qwen2.5 JSON / Qwen3.5 XML <tool_call> output
#   - vlib_video_session   : continuous-frame session (KV-cache-aware)
#   - vlib_video_encoder   : pluggable per-frame encoder (pair2d, conv3d)
#
# Static library only; consumers (mtmd-video-cli, server video shim, third
# party) link against `vlib-video`. Mirrors the cross-project layout described
# in VIDEO_CONV3D_CPP_DESIGN.md so it can be cherry-picked into
# ik_llama.cpp's `examples/vlib-video/` with minimal fuss.

find_package(Threads REQUIRED)

add_library(vlib-video STATIC
    vlib_video_tool_parser.cpp
    vlib_video_session.cpp
    vlib_video_encoder.cpp
    vlib_video.h
    vlib_video_tool_parser.h
    vlib_video_session.h
    vlib_video_encoder.h
)

set_target_properties(vlib-video PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(vlib-video PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(vlib-video PRIVATE ${CMAKE_SOURCE_DIR})

target_link_libraries(vlib-video PUBLIC  mtmd llama ggml)
target_link_libraries(vlib-video PRIVATE Threads::Threads)

target_compile_features(vlib-video PRIVATE cxx_std_17)

if (LLAMA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
