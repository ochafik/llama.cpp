set(TARGET simple)
add_example(${TARGET} simple.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})
