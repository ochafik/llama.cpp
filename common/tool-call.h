#pragma once

#include "ggml.h"
// Change JSON_ASSERT from assert() to GGML_ASSERT:
#define JSON_ASSERT GGML_ASSERT
#include "json.hpp"

struct llama_tool_call {
    std::string name;
    std::string arguments;
};

struct llama_tool_calls {
    std::string content;
    std::vector<llama_tool_call> tool_calls;
};

struct llama_tool_call_handler {
    std::string grammar;
    std::vector<std::string> grammar_trigger_words;
    std::vector<std::string> additional_stop_words;
};

llama_tool_calls parse_tool_calls(const nlohmann::ordered_json & tools, const std::string & chat_template, const std::string& input);

llama_tool_call_handler llama_tool_call_handler_init(
    const std::string & chat_template,
    bool allow_content,
    bool parallel_tool_calls,
    const nlohmann::ordered_json & tools);
