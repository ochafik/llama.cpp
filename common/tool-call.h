#pragma once

#include "ggml.h"
// Change JSON_ASSERT from assert() to GGML_ASSERT:
#define JSON_ASSERT GGML_ASSERT
#include "json.hpp"

std::pair<std::string, nlohmann::ordered_json> parse_tool_calls(const nlohmann::ordered_json & tools, const std::string & chat_template, const std::string& input);
