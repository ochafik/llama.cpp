set(TARGET llama-cpp)
add_executable(${TARGET} cli.cpp)
install(TARGETS ${TARGET} RUNTIME)
target_link_libraries(${TARGET} PRIVATE
    main
    server
    embedding
    llava-cli
    quantize
    tokenize
    finetune
    train-text-from-scratch
    speculative
    perplexity
    lookahead
    lookup
    lookup-create
    lookup-merge
    lookup-stats
    imatrix
)
target_compile_features(${TARGET} PRIVATE cxx_std_11)
