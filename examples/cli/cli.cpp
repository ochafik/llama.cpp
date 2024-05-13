#include <map>
#include <stdio.h>
#include <string>

int main_main(int argc, char ** argv);
int server_main(int argc, char ** argv);
int embedding_main(int argc, char ** argv);
int llava_cli_main(int argc, char ** argv);
int quantize_main(int argc, char ** argv);
int tokenize_main(int argc, char ** argv);
int finetune_main(int argc, char ** argv);
int train_text_from_scratch_main(int argc, char ** argv);
int speculative_main(int argc, char ** argv);
int perplexity_main(int argc, char ** argv);
int lookahead_main(int argc, char ** argv);
int lookup_main(int argc, char ** argv);
int lookup_create_main(int argc, char ** argv);
int lookup_merge_main(int argc, char ** argv);
int lookup_stats_main(int argc, char ** argv);
int imatrix_main(int argc, char ** argv);
int baby_llama_main(int argc, char ** argv);
int batched_bench_main(int argc, char ** argv);
int batched_main(int argc, char ** argv);
int beam_search_main(int argc, char ** argv);
int benchmark_main(int argc, char ** argv);
int convert_llama2c_to_ggml_main(int argc, char ** argv);
int eval_callback_main(int argc, char ** argv);
int export_lora_main(int argc, char ** argv);
int gbnf_validator_main(int argc, char ** argv);
int gguf_split_main(int argc, char ** argv);
int gritlm_main(int argc, char ** argv);
int infill_main(int argc, char ** argv);
int llama_bench_main(int argc, char ** argv);
int parallel_main(int argc, char ** argv);
int passkey_main(int argc, char ** argv);
int quantize_stats_main(int argc, char ** argv);
int retrieval_main(int argc, char ** argv);

int main(int argc, char ** argv) {
    struct Command {
        std::string description;
        int (*main)(int argc, char ** argv);
    };
    const std::map<std::string, Command> commands {
        {"run", {"Run a model in chat mode", main_main}},
        {"serve", {"Serves a model on http://localhost:8080 (web interface + OpenAI-compatible endpoint)", server_main}},
        {"embedding", {"Embedding mode", embedding_main}},
        {"llava", {"", llava_cli_main}},
        {"quantize", {"", quantize_main}},
        {"tokenize", {"", tokenize_main}},
        {"finetune", {"", finetune_main}},
        {"train-text-from-scratch", {"", train_text_from_scratch_main}},
        {"speculative", {"", speculative_main}},
        {"perplexity", {"", perplexity_main}},
        {"lookahead", {"", lookahead_main}},
        {"lookup", {"", lookup_main}},
        {"lookup-create", {"", lookup_create_main}},
        {"lookup-merge", {"", lookup_merge_main}},
        {"lookup-stats", {"", lookup_stats_main}},
        {"imatrix", {"", imatrix_main}},
        {"baby-llama", {"", baby_llama_main}},
        {"batched-bench", {"", batched_bench_main}},
        {"batched", {"", batched_main}},
        {"beam-search", {"", beam_search_main}},
        {"benchmark", {"", benchmark_main}},
        {"convert-llama2c-to-ggml", {"", convert_llama2c_to_ggml_main}},
        {"eval-callback", {"", eval_callback_main}},
        {"export-lora", {"", export_lora_main}},
        {"gbnf-validator", {"", gbnf_validator_main}},
        {"gguf-split", {"", gguf_split_main}},
        {"gritlm", {"", gritlm_main}},
        {"infill", {"", infill_main}},
        {"llama-bench", {"", llama_bench_main}},
        {"parallel", {"", parallel_main}},
        {"passkey", {"", passkey_main}},
        {"quantize-stats", {"", quantize_stats_main}},
        {"retrieval", {"", retrieval_main}},
    };
    if (argc == 1) {
        fprintf(stderr, "[%s]\n", argv[0]);
        fprintf(stderr, "Usage: %s <command> <command args>\n", argv[0]);
        fprintf(stderr, "Available commands:\n");
        for (const auto & pair : commands) {
            fprintf(stderr, "  %s: %s\n", pair.first.c_str(), pair.second.description.c_str());
        }
        fprintf(stderr, "Examples:\n");
        fprintf(stderr, "  - Run a model in chat mode:\n");
        fprintf(stderr, "    %s run -mu https://......gguf -clm\n", argv[0]);
        fprintf(stderr, "  - Serves a model on http://localhost:8080 (web interface + OpenAI-compatible endpoint)\n");
        fprintf(stderr, "    %s serve -mu https://......gguf\n", argv[0]);
        fprintf(stderr, "  - Embedding mode\n");
        fprintf(stderr, "    %s embedding -mu https://......gguf\n", argv[0]);
        return 1;
    }
    std::string command = argv[1];
    auto it = commands.find(command);
    if (it == commands.end()) {
        fprintf(stderr, "Unknown command: %s\n", command.c_str());
        return 1;
    }
    return it->second.main(argc - 1, argv + 1);
}