#pragma once
#include "json.hpp"

nlohmann::ordered_json tool_call_schema(const nlohmann::ordered_json & tools, const nlohmann::ordered_json & response_schema, bool allow_parallel_calls = false);
std::string tool_call_grammar(const nlohmann::ordered_json & tools, bool allow_parallel_calls = false);
std::string json_schema_to_grammar(const nlohmann::ordered_json& schema);
