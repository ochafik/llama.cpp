set(TARGET llama-cpp)
add_executable(${TARGET} cli.cpp)
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
    baby-llama
    batched-bench
    batched
    beam-search
    benchmark
    convert-llama2c-to-ggml
    eval-callback
    export-lora
    gbnf-validator
    gguf-split
    gritlm
    infill
    llama-bench
    parallel
    passkey
    quantize-stats
    retrieval
)
target_compile_features(${TARGET} PRIVATE cxx_std_11)
