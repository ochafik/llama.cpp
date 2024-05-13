set(TARGET parallel)
add_example(${TARGET} parallel.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})
