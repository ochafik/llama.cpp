set(TARGET gbnf-validator)
add_example(${TARGET} gbnf-validator.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})
