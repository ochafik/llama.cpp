set(TARGET gguf)
add_example(${TARGET} gguf.cpp)
target_link_libraries(${TARGET} PRIVATE ggml ${CMAKE_THREAD_LIBS_INIT})
