# mtmd-video-cli — minimal driver for the vlib-video continuous session.
#
# Usage (v1):
#   llama-mtmd-video-cli -m TEXT.gguf --mmproj MMPROJ.gguf \
#                        --frames-dir DIR --fps 0.5 \
#                        --system-prompt "..."
#
# We deliberately avoid linking against OpenCV — frames are read as JPEG/PNG
# files from a directory in alphabetical order and decoded via stb_image
# (already vendored under llama.cpp/vendor/).

set(TARGET llama-mtmd-video-cli)
add_executable(${TARGET} mtmd-video-cli.cpp)
set_target_properties(${TARGET} PROPERTIES OUTPUT_NAME llama-mtmd-video-cli)

target_link_libraries(${TARGET} PRIVATE
    llama-common
    mtmd
    vlib-video
    Threads::Threads
)

target_include_directories(${TARGET} PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/vendor
)

target_compile_features(${TARGET} PRIVATE cxx_std_17)

if (LLAMA_TOOLS_INSTALL)
    install(TARGETS ${TARGET} RUNTIME)
endif()
