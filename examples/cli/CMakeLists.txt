set(TARGET llama-cpp)
add_executable(${TARGET} cli.cpp)
target_link_libraries(${TARGET} PRIVATE
    # baby-llama
    batched
    batched-bench
    beam-search
    benchmark
    convert-llama2c-to-ggml
    embedding
    eval-callback
    export-lora
    finetune
    gbnf-validator
    gguf
    gguf-split
    gritlm
    imatrix
    infill
    llama-bench
    llava-cli
    lookahead
    lookup
    lookup-create
    lookup-merge
    lookup-stats
    main
    parallel
    passkey
    perplexity
    quantize
    quantize-stats
    retrieval
    server
    speculative
    tokenize
    train-text-from-scratch
)
target_compile_features(${TARGET} PRIVATE cxx_std_11)
