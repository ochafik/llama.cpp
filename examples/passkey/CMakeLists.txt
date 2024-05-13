set(TARGET passkey)
add_example(${TARGET} passkey.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})
