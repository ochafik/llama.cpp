#pragma once

#include "ggml.h"
// Change JSON_ASSERT from assert() to GGML_ASSERT:
#define JSON_ASSERT GGML_ASSERT
#include "json.hpp"

std::pair<std::string, nlohmann::ordered_json> parse_tool_calls(const nlohmann::ordered_json & tools, const std::string & chat_template, const std::string& input);

void tool_call_grammar(
    const std::string & chat_template,
    bool allow_content,
    bool parallel_tool_calls,
    const nlohmann::ordered_json & tools,
    std::string & grammar,
    std::vector<std::string> & grammar_trigger_words,
    std::vector<std::string> & additional_stop_words,
    std::function<bool(std::string::const_iterator &, const std::string::const_iterator &, nlohmann::ordered_json &)> & tool_call_parser);
