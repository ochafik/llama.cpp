set(TARGET lookup)
add_example(${TARGET} lookup.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})

set(TARGET lookup-create)
add_example(${TARGET} lookup-create.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})

set(TARGET lookup-merge)
add_example(${TARGET} lookup-merge.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})

set(TARGET lookup-stats)
add_example(${TARGET} lookup-stats.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})
