set(TARGET train-text-from-scratch)
add_example(${TARGET} train-text-from-scratch.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})
target_compile_features(${TARGET} PRIVATE cxx_std_11)
