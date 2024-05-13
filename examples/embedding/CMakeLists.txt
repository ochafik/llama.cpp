set(TARGET embedding)
add_example(${TARGET} embedding.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})
