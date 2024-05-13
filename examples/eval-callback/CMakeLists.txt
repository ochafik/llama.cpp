set(TARGET eval-callback)
add_example(${TARGET} eval-callback.cpp)
target_link_libraries(${TARGET} PRIVATE common llama ${CMAKE_THREAD_LIBS_INIT})

set(TEST_TARGET test-eval-callback)
add_test(NAME ${TEST_TARGET} COMMAND eval-callback --hf-repo ggml-org/models --hf-file tinyllamas/stories260K.gguf --model stories260K.gguf --prompt hello --seed 42 -ngl 0)
set_property(TEST ${TEST_TARGET} PROPERTY LABELS eval-callback curl)
