set(TARGET finetune)
add_example(${TARGET} finetune.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})
