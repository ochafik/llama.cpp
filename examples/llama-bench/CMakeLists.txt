set(TARGET llama-bench)
add_example(${TARGET} llama-bench.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})
