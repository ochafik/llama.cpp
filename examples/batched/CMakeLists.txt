set(TARGET batched)
add_example(${TARGET} batched.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})
