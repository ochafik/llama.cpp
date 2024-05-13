set(TARGET export-lora)
add_example(${TARGET} export-lora.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})
