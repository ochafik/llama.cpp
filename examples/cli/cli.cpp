#include <map>
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
            entry_point main;
        };
        std::map<std::string, Command> commands;

        #define REGISTER_COMMAND(name, symbol, is_core, description) \
            int symbol ## _main(int argc, char ** argv); \
            commands[name] = {is_core, description, symbol ## _main};

        // Core commands
        commands["commands"] = {true, "List all available commands", nullptr};
        REGISTER_COMMAND("embed",          embedding, true, "Embedding mode");
        REGISTER_COMMAND("llava",          llava_cli, true, "");
        REGISTER_COMMAND("quantize",       quantize, true, "");
        REGISTER_COMMAND("run",            main, true, "Run a model in chat mode");
        REGISTER_COMMAND("serve",          server, true, "Serves a model on http://localhost:8080 (web interface + OpenAI-compatible endpoint)");
        REGISTER_COMMAND("bench",          llama_bench, true, "");

        // Other commands
        REGISTER_COMMAND("baby",           baby_llama, false, "");
        REGISTER_COMMAND("batched",        batched, false, "");
        REGISTER_COMMAND("beam-search",    beam_search, false, "");
        REGISTER_COMMAND("bench-batched",  batched_bench, false, "");
        REGISTER_COMMAND("bench-matmult",  benchmark, false, "");
        REGISTER_COMMAND("convert-llama2c",convert_llama2c_to_ggml, false, "");
        REGISTER_COMMAND("eval_callback",  eval_callback, false, "");
        REGISTER_COMMAND("export_lora",    export_lora, false, "");
        REGISTER_COMMAND("finetune",       finetune, false, "");
        REGISTER_COMMAND("gbnf-validate",  gbnf_validator, false, "");
        REGISTER_COMMAND("gguf-split",     gguf_split, false, "");
        REGISTER_COMMAND("gritlm",         gritlm, false, "");
        REGISTER_COMMAND("imatrix",        imatrix, false, "");
        REGISTER_COMMAND("infill",         infill, false, "");
        REGISTER_COMMAND("lookahead",      lookahead, false, "");
        REGISTER_COMMAND("lookup",         lookup, false, "");
        REGISTER_COMMAND("lookup-create",  lookup_create, false, "");
        REGISTER_COMMAND("lookup-merge",   lookup_merge, false, "");
        REGISTER_COMMAND("lookup-stats",   lookup_stats, false, "");
        REGISTER_COMMAND("parallel",       parallel, false, "");
        REGISTER_COMMAND("passkey",        passkey, false, "");
        REGISTER_COMMAND("perplexity",     perplexity, false, "");
        REGISTER_COMMAND("quantize-stats", quantize_stats, false, "");
        REGISTER_COMMAND("retrieval",      retrieval, false, "");
        REGISTER_COMMAND("speculate",      speculative, false, "");
        REGISTER_COMMAND("tokenize",       tokenize, false, "");
        REGISTER_COMMAND("train",          train_text_from_scratch, false, "");

        auto print_commands = [&](bool core_only) {
            std::string program_name(argv[0]);
            auto it = program_name.find_last_of('/');
            if (it != std::string::npos) {
                program_name = program_name.substr(it + 1);
            }

            fprintf(stderr, "%s", BANNER);
            fprintf(stderr, "Usage: %s <command> <command args>\n\n", program_name.c_str());

            auto print_command = [&](const std::string & name, const Command & command) {
                fprintf(stderr, "  %s: %s\n", name.c_str(), command.description.c_str());
            };

            fprintf(stderr, "Core commands:\n");
            for (const auto & pair : commands) {
                if (pair.second.is_core) {
                    print_command(pair.first, pair.second);
                }
            }
            fprintf(stderr, "\n");

            if (!core_only) {
                fprintf(stderr, "Other commands:\n");
                for (const auto & pair : commands) {
                    if (!pair.second.is_core) {
                        print_command(pair.first, pair.second);
                    }
                }
                fprintf(stderr, "\n");
            }

            fprintf(stderr, "Examples:\n");
            fprintf(stderr, "  - Run a model in chat mode:\n");
            fprintf(stderr, "    %s run -mu https://......gguf -clm\n", program_name.c_str());
            fprintf(stderr, "  - Serves a model on http://localhost:8080 (web interface + OpenAI-compatible endpoint)\n");
            fprintf(stderr, "    %s serve -mu https://......gguf\n", program_name.c_str());
            fprintf(stderr, "  - Embedding mode\n");
            fprintf(stderr, "    %s embedding -mu https://......gguf\n", program_name.c_str());
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