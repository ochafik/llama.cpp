set(TARGET quantize)
add_example(${TARGET} quantize.cpp)
target_link_libraries(${TARGET} PRIVATE llama common ${CMAKE_THREAD_LIBS_INIT})
target_include_directories(${TARGET} PRIVATE ../../common)
