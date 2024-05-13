set(TARGET train-text-from-scratch)
add_example(${TARGET} train-text-from-scratch.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})
