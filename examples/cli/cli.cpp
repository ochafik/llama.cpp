#include <map>
#include <regex>
#include <stdio.h>
#include <string>

const char * BANNER = R"""(
     _ _
    | | | __ _ _ __ ___   __ _   ___ _ __  _ __
    | | |/ _` | '_ ` _ \ / _` | / __| '_ \| '_ \
    | | | (_| | | | | | | (_| || (__| |_) | |_) |
    |_|_|\__,_|_| |_| |_|\__,_(_)___| .__/| .__/
                                    |_|   |_|

)""";

int main(int argc, char ** argv) {
    typedef int (*entry_point)(int argc, char ** argv);

    entry_point main = nullptr;
    // Explicit scope for command registration / lookup.
    {
        struct Command {
            bool is_core;
            std::string description;
            std::string url;
            entry_point main;
        };
        std::map<std::string, Command> commands;

        auto get_url = [&](const std::string & symbol) {
            return std::string("https://github.com/ggerganov/llama.cpp/tree/master/examples/")
                + std::regex_replace(symbol, std::regex("_"), "-");
        };

        #define REGISTER_COMMAND(name, symbol, is_core, description, url) \
            int symbol ## _main(int argc, char ** argv); \
            commands[name] = {is_core, description, strlen(url) == 0 ? get_url(#symbol) : url, symbol ## _main};

        // Core commands
        commands["commands"] = {true, "List all available commands", "", nullptr};

        REGISTER_COMMAND("embed",          embedding, true, "Embedding mode", "");
        REGISTER_COMMAND("llava",          llava_cli, true, "Performs generation with LLaVA (Large Language-and-Vision Assistant)", "");
        REGISTER_COMMAND("gguf",           gguf, true, "Read / write a GGUF file", "");
        REGISTER_COMMAND("quantize",       quantize, true, "Quantizes a model", "");
        REGISTER_COMMAND("run",            main, true, "Run a model in chat mode", "");
        REGISTER_COMMAND("serve",          server, true, "Serves a model on http://localhost:8080 (Web interface + OpenAI-compatible endpoint)", "");
        REGISTER_COMMAND("bench",          llama_bench, true, "Performance testing tool for llama.cpp", "");

        // Other commands
        REGISTER_COMMAND("batched",        batched, false, "Demo of batched generation from a given prompt", "");
        REGISTER_COMMAND("beam-search",    beam_search, false, "Performs beam search decoding (see https://github.com/ggerganov/llama.cpp/pull/2267)", "");
        REGISTER_COMMAND("bench-batched",  batched_bench, false, "Benchmark the batched decoding performance of `llama.cpp`", "");
        REGISTER_COMMAND("bench-matmult",  benchmark, false, "Benmark matrix multiplication performance", "");
        REGISTER_COMMAND("convert-llama2c",convert_llama2c_to_ggml, false, "Convert llama2.c weights to GGUF", "");
        REGISTER_COMMAND("eval-callback",  eval_callback, false, "Prints to the console all operations and tensor data (demonstrates inference callbacks usage).", "");
        REGISTER_COMMAND("export-lora",    export_lora, false, "Applies LORA adapters to a base model and exports the resulting model.", "");
        REGISTER_COMMAND("finetune",       finetune, false, "Fine-tines a LORA adapter", "");
        REGISTER_COMMAND("gbnf-validate",  gbnf_validator, false, "Validates an text against a given grammar", "");
        REGISTER_COMMAND("gguf-split",     gguf_split, false, "CLI to split / merge GGUF files", "");
        REGISTER_COMMAND("gritlm",         gritlm, false, "Generative Representational Instruction Tuning (GRIT) Example", "");
        REGISTER_COMMAND("imatrix",        imatrix, false, "Compute an importance matrix for a model and given text dataset", "");
        REGISTER_COMMAND("infill",         infill, false, "Demo of infill mode with Code Llama models", "");
        REGISTER_COMMAND("lookahead",      lookahead, false, "Demo of lookahead decoding technique", "");
        REGISTER_COMMAND("lookup",         lookup, false, "Demo of Prompt Lookup Decoding", "");
        REGISTER_COMMAND("lookup-create",  lookup_create, false, "For use w/ lookup command (doc needed)", "");
        REGISTER_COMMAND("lookup-merge",   lookup_merge, false, "For use w/ lookup command (doc needed)", "");
        REGISTER_COMMAND("lookup-stats",   lookup_stats, false, "For use w/ lookup command (doc needed)", "");
        REGISTER_COMMAND("parallel",       parallel, false, "Simplified simulation of serving incoming requests in parallel", "");
        REGISTER_COMMAND("passkey",        passkey, false, "Tests a model's abiltiy to find a needle 'passkey' in a haystack", "https://github.com/ggerganov/llama.cpp/pull/3856");
        REGISTER_COMMAND("perplexity",     perplexity, false, "Calculates the so-called perplexity value of a language model over a given text corpus.", "");
        REGISTER_COMMAND("quantize-stats", quantize_stats, false, "Prints quantization statistics for a model", "");
        REGISTER_COMMAND("retrieval",      retrieval, false, "Demo of simple retrieval technique based on cosine similarity", "");
        REGISTER_COMMAND("speculate",      speculative, false, "Demo of speculative decoding and tree-based speculative decoding techniques", "");
        REGISTER_COMMAND("tokenize",       tokenize, false, "Tokenizes a prompt", "");
        REGISTER_COMMAND("train",          train_text_from_scratch, false, "Train a text model from scratch", "");
        // REGISTER_COMMAND("train-baby",     baby_llama, false, "Example of training a baby llama model", "https://github.com/ggerganov/llama.cpp/pull/1360");

        auto print_commands = [&](bool core_only) {
            std::string program_name(argv[0]);
            auto it = program_name.find_last_of('/');
            if (it != std::string::npos) {
                program_name = program_name.substr(it + 1);
            }

            fprintf(stderr, "%s", BANNER);
            fprintf(stderr, "Usage: %s <command> <command args>\n\n", program_name.c_str());

            auto print_command = [&](const std::string & name, const Command & command) {
                fprintf(stderr, "  %s:\n    %s\n    See %s\n\n", name.c_str(), command.description.c_str(), command.url.c_str());
            };

            fprintf(stderr, "Core commands:\n\n");
            for (const auto & pair : commands) {
                if (pair.second.is_core) {
                    print_command(pair.first, pair.second);
                }
            }

            if (!core_only) {
                fprintf(stderr, "Other commands:\n\n");
                for (const auto & pair : commands) {
                    if (!pair.second.is_core) {
                        print_command(pair.first, pair.second);
                    }
                }
            }

            fprintf(stderr, "Examples:\n\n");
            fprintf(stderr, "  - Run a model in chat mode:\n");
            fprintf(stderr, "    %s run -clm -hfr microsoft/Phi-3-mini-4k-instruct-gguf -hff Phi-3-mini-4k-instruct-q4.gguf\n\n", program_name.c_str());
            fprintf(stderr, "  - Serves a model on http://localhost:8080 (web interface + OpenAI-compatible endpoint)\n");
            fprintf(stderr, "    %s serve    -hfr microsoft/Phi-3-mini-4k-instruct-gguf -hff Phi-3-mini-4k-instruct-q4.gguf\n\n", program_name.c_str());
            fprintf(stderr, "  - Embedding mode\n");
            fprintf(stderr, "    %s embed    -hfr microsoft/Phi-3-mini-4k-instruct-gguf -hff Phi-3-mini-4k-instruct-q4.gguf\n", program_name.c_str());
            fprintf(stderr, "\n");
        };
        if (argc == 1) {
            fprintf(stderr, "ERROR: No command specified\n");
            print_commands(/* core_only= */ true);
            return 1;
        }
        std::string command = argv[1];
        if (command == "--help") {
            print_commands(/* core_only= */ true);
            return 0;
        } else if (command == "commands") {
            print_commands(/* core_only= */ false);
            return 0;
        }
        auto it = commands.find(command);
        if (it == commands.end()) {
            fprintf(stderr, "ERROR: Unknown command: %s\n", command.c_str());
            print_commands(/* core_only= */ true);
            return 1;
        }
        main = it->second.main;
    }
    return main(argc - 1, argv + 1);
}