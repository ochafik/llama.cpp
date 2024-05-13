set(TARGET perplexity)
add_example(${TARGET} perplexity.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})
