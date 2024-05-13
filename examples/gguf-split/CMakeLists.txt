set(TARGET gguf-split)
add_example(${TARGET} gguf-split.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})
