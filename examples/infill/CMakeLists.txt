set(TARGET infill)
add_example(${TARGET} infill.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})
