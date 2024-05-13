set(TARGET mainlib)
add_library(${TARGET} main.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})
target_compile_features(${TARGET} PRIVATE cxx_std_11)

if (NOT LLAMA_CLI)
    set(TARGET main)
    add_executable(${TARGET})
    install(TARGETS ${TARGET} RUNTIME)
    target_link_libraries(${TARGET} PRIVATE mainlib)
endif()

