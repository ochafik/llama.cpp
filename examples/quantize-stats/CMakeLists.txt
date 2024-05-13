set(TARGET quantize-stats)
add_example(${TARGET} quantize-stats.cpp)
target_link_libraries(${TARGET} PRIVATE llama build_info ${CMAKE_THREAD_LIBS_INIT})
target_include_directories(${TARGET} PRIVATE ../../common)

