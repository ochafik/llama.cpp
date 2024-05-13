set(TARGET imatrix)
add_example(${TARGET} imatrix.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})
