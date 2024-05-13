set(TARGET beam-search)
add_example(${TARGET} beam-search.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})
