#include "tool-call.h"
#include "json-schema-to-grammar.h"
#include <algorithm>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using json = nlohmann::ordered_json;

static bool parse_json(std::string::const_iterator & it, const std::string::const_iterator & end, json & out) {
    // // https://json.nlohmann.me/features/parsing/sax_interface/
    struct json_error_locator : public nlohmann::json_sax<json> {
        std::size_t position;
        bool found_error;

        bool parse_error(std::size_t position, const std::string & last_token, const json::exception & ex) override {
            // LOG_WARNING("JSON error (Expected)", {{"position", position}, {"last_token", last_token}, {"error", ex.what()}});
            this->position = position - 1;
            this->found_error = true;
            return false;
        }
        bool null() override { return true; }
        bool boolean(bool) override { return true; }
        bool number_integer(number_integer_t) override { return true; }
        bool number_unsigned(number_unsigned_t) override { return true; }
        bool number_float(number_float_t, const string_t &) override { return true; }
        bool string(string_t &) override { return true; }
        bool binary(binary_t &) override { return true; }
        bool start_object(std::size_t) override { return true; }
        bool key(string_t &) override { return true; }
        bool end_object() override { return true; }
        bool start_array(std::size_t) override { return true; }
        bool end_array() override { return true; }
    };
    json_error_locator err_loc;
    json::sax_parse(it, end, &err_loc);

    std::string::const_iterator temptative_end;
    if (err_loc.found_error) {
        temptative_end = it + err_loc.position;
    } else {
        temptative_end = end;
    }
    std::string json_sub {it, it + err_loc.position};
    // LOG_WARNING("Parsing json", {{"json_sub", json_sub}});
    try {
        out = json::parse(json_sub);
        it = temptative_end;
        return true;
    } catch (const std::exception & e) {
        // LOG_WARNING("Failed to parse tool call", {{"json_sub", json_sub}, {"error", e.what()}});
        return false;
    }
}

static std::pair<std::string, json> parse_hermes_tool_calls(const std::string& input) {
    try {
        std::regex start_pattern(R"(^[\n\s]*<tool_call>)");
        std::regex middle_pattern(R"([\n\s]*</tool_call>[\n\s]*<tool_call>)");
        std::regex end_pattern(R"([\n\s]*</tool_call>[\n\s]*$)");
        
        auto end = input.end();
        std::sregex_iterator rend;
        std::sregex_iterator rit(input.begin(), end, start_pattern);
        if (rit == rend) {
            return {input, json()};
        }
        
        auto content = rit->prefix();

        json tool_calls = json::array();
        auto it = rit->suffix().first;
        while (it != end) {
            json call;
            if (!parse_json(it, end, call)) {
                // LOG_WARNING("Failed to parse json tool call", {{"input", input}});
                throw std::runtime_error("Failed to parse json tool call");
                // break;
            }
            tool_calls.push_back({
                {"function", {
                    {"name", call["name"]},
                    {"arguments", call["arguments"].dump()},
                }},
            });
            rit = {it, end, middle_pattern};
            if (rit != rend) {
                it = rit->suffix().first;
            } else {
                rit = {it, end, end_pattern};
                if (rit == rend) {
                    // LOG_WARNING("Malformed input, missing </tool_call>", {{"input", input}});
                    throw std::runtime_error("Malformed input, missing </tool_call>");
                }
                break;
            }
        }
        return {content, tool_calls};
    } catch (const std::exception & e) {
        // LOG_WARNING("Failed to parse tool calls", {{"input", input}, {"error", e.what()}});
        return {input, json()};
    }
}

static std::pair<std::string, json> parse_llama_3_1_tool_calls(const json & tools, const std::string& input) {
    static std::regex python_tag_regex(R"(^<\|python_tag\|>(.*)$)");
    std::smatch match;
    if (std::regex_search(input, match, python_tag_regex)) {
        return {match.prefix().str(), {{
            {"function", {
                {"name", "ipython"},
                {"arguments", (json {
                    {"code", match[1].str()},
                }).dump()},
            }}
        }}};
    }
    try {
        auto call = json::parse(input);
        // Only treat JSON as a tool call if it has a name attribute that matches any of the tools specified in the request.
        // There doesn't seem to be any better way to detect a tool call.
        if (call.contains("name") && call["name"].is_string()) {
            std::string name = call["name"];
            for (const auto & tool : tools) {
                if (tool.at("function").at("name") == name) {
                    return {"", {{
                        {"function", {
                            {"name", name},
                            {"arguments", call["parameters"].dump()},
                        
                        }},
                    }}};
                }
            }
        }
    } catch (const std::exception & e) {
        // Do nothing
    }
    return {input, json()};
}


static std::pair<std::string, json> parse_functionary_3_2_tool_calls(const std::string& input) {
    static std::regex python_tag_regex(R"(>>>(\w+)\n((?!>>>).+))");
    std::smatch match;
    json tool_calls = json::array();
    std::string content;
    std::string in = input;
    while (std::regex_search(in, match, python_tag_regex)) {
        content += match.prefix().str();
        tool_calls.push_back({
            {"function", {
                {"name", match[1].str()},
                {"arguments", (json {
                    {"code", match[2].str()}
                }).dump()},
            }},
        });
        in = match.suffix().str();
    }
    return {content, tool_calls};
}

std::pair<std::string, json> parse_tool_calls(const json & tools, const std::string & chat_template, const std::string& input) {
    if (chat_template.find("<tool_call>") != std::string::npos) {
        return parse_hermes_tool_calls(input);
    } else if (chat_template.find("<|start_header_id|>") != std::string::npos
            && chat_template.find("<|python_tag|>") != std::string::npos) {
        return parse_llama_3_1_tool_calls(tools, input);
    } else if (chat_template.find("<|start_header_id|>") != std::string::npos
            && chat_template.find(">>>all") != std::string::npos) {
        return parse_functionary_3_2_tool_calls(input);
    } else {
        throw std::runtime_error("Unsupported chat template for tool calls");
    }
}
