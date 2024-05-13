set(TARGET speculative)
add_example(${TARGET} speculative.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})

