set(TARGET gritlm)
add_example(${TARGET} gritlm.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})
