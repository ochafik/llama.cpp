#pragma once

#include "ggml.h"
// Change JSON_ASSERT from assert() to GGML_ASSERT:
#define JSON_ASSERT GGML_ASSERT
#include "json.hpp"

void tool_call_grammar(
    const std::string & chat_template,
    bool allow_content,
    bool parallel_tool_calls,
    const nlohmann::ordered_json & tools,
    std::string & grammar,
    std::vector<std::string> & grammar_trigger_words,
    std::vector<std::string> & additional_stop_words,
    std::function<bool(std::string::const_iterator &, const std::string::const_iterator &, nlohmann::ordered_json &)> & tool_call_parser);

std::string json_schema_to_grammar(const nlohmann::ordered_json& schema);
