set(TARGET batched-bench)
add_example(${TARGET} batched-bench.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})
