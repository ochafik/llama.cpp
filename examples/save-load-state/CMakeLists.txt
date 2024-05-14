set(TARGET save-load-state)
add_example(${TARGET} save-load-state.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})
