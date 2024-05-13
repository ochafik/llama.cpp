set(TARGET tokenize)
add_example(${TARGET} tokenize.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})
