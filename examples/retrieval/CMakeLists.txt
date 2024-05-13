set(TARGET retrieval)
add_example(${TARGET} retrieval.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})
