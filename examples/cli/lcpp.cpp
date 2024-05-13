#include <stdio.h>
#include <string>

int main_main(int argc, char ** argv);
int server_main(int argc, char ** argv);

int main(int argc, char ** argv) {
    if (argc == 1) {
        fprintf(stderr, "llama.cpp\n");
        fprintf(stderr, "Usage: lcpp <command> <command args>\n");
        fprintf(stderr, "Available commands:\n");
        fprintf(stderr, "  run\n");
        fprintf(stderr, "  serve\n");
        fprintf(stderr, "Examples:\n");
        fprintf(stderr, "  - Run a model in chat mode:\n");
        fprintf(stderr, "    lcpp run -mu https://......gguf -clm\n");
        fprintf(stderr, "  - Serves a model on http://localhost:8080 (web interface + OpenAI-compatible endpoint)\n");
        fprintf(stderr, "    lcpp serve -mu https://......gguf\n");
        return 1;
    }
    std::string command = argv[1];
    if (command == "serve") {
        return server_main(argc - 1, argv + 1);
    } else if (command == "run") {
        return main_main(argc - 1, argv + 1);
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        return 1;
    }
}