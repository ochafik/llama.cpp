#include "chat-peg-parser.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using Tag = common_chat_peg_tag;

static std::string_view trim_trailing_space(std::string_view sv, int max = -1) {
    int count = 0;
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
        if (max != -1 && count <= max) {
            break;
        }
        sv.remove_suffix(1);
        count++;
    }
    return sv;
}

common_chat_peg_mapper common_chat_peg_base_mapper() {
    return [](common_chat_msg & result) -> common_chat_peg_map_func {
        return [&result](const common_peg_ast_node & node) {
            switch (static_cast<Tag>(node.tag_id)) {
                case Tag::REASONING:
                    result.reasoning_content += std::string(trim_trailing_space(node.text));
                    break;
                case Tag::CONTENT:
                    result.content += std::string(trim_trailing_space(node.text));
                    break;
                default:
                    break;
            }
        };
    };
}

common_chat_peg_mapper common_chat_peg_native_mapper() {
    return [](common_chat_msg & result) -> common_chat_peg_map_func {
        auto base = common_chat_peg_base_mapper()(result);
        common_chat_tool_call * current_tool = nullptr;

        return [&result, base, current_tool](const common_peg_ast_node & node) mutable {
            base(node);

            switch (static_cast<Tag>(node.tag_id)) {
                case Tag::TOOL_OPEN:
                    result.tool_calls.emplace_back();
                    current_tool = &result.tool_calls.back();
                    break;
                case Tag::TOOL_ID:
                    if (current_tool) {
                        current_tool->id = std::string(trim_trailing_space(node.text));
                    }
                    break;
                case Tag::TOOL_NAME:
                    if (current_tool) {
                        current_tool->name = std::string(trim_trailing_space(node.text));
                    }
                    break;
                case Tag::TOOL_ARGS:
                    if (current_tool) {
                        current_tool->arguments = std::string(trim_trailing_space(node.text));
                    }
                    break;
                default:
                    break;
            }
        };
    };
}

common_chat_peg_mapper common_chat_peg_constructed_mapper() {
    return [](common_chat_msg & result) -> common_chat_peg_map_func {
        auto base = common_chat_peg_base_mapper()(result);
        common_chat_tool_call * current_tool = nullptr;
        int arg_count = 0;
        bool needs_closing_quote = false;
        bool args_complete = false;  // True if TOOL_ARGS set complete arguments

        return [&result, base, current_tool, arg_count, needs_closing_quote, args_complete](const common_peg_ast_node & node) mutable {
            base(node);

            switch (static_cast<Tag>(node.tag_id)) {
                case Tag::TOOL_OPEN:
                    result.tool_calls.emplace_back();
                    current_tool = &result.tool_calls.back();
                    arg_count = 0;
                    args_complete = false;
                    break;
                case Tag::TOOL_NAME:
                    if (current_tool) {
                        current_tool->name = std::string(node.text);
                        current_tool->arguments = "{";
                    }
                    break;
                case Tag::TOOL_ARG_OPEN:
                    needs_closing_quote = false;
                    break;
                case Tag::TOOL_ARG_NAME:
                    if (current_tool) {
                        if (arg_count > 0) {
                            current_tool->arguments += ",";
                        }
                        current_tool->arguments += json(trim_trailing_space(node.text)).dump() + ":";
                        ++arg_count;
                    }
                    break;
                case Tag::TOOL_ARG_STRING_VALUE:
                    if (current_tool) {
                        // Serialize to JSON, but exclude the end quote
                        std::string dumped = json(node.text).dump();
                        current_tool->arguments += dumped.substr(0, dumped.size() - 1);
                        needs_closing_quote = true;
                    }
                    break;
                case Tag::TOOL_ARG_CLOSE:
                    if (current_tool && needs_closing_quote) {
                        current_tool->arguments += "\"";
                        needs_closing_quote = false;
                    }
                    break;
                case Tag::TOOL_ARG_JSON_VALUE:
                    if (current_tool) {
                        current_tool->arguments += std::string(trim_trailing_space(node.text));
                    }
                    break;
                case Tag::TOOL_ARGS:
                    // For formats that use both constructed args and complete JSON args
                    // (e.g., Llama 3.x with builtin tools), replace the arguments entirely
                    if (current_tool) {
                        current_tool->arguments = std::string(trim_trailing_space(node.text));
                        args_complete = true;
                    }
                    break;
                case Tag::TOOL_CLOSE:
                    if (current_tool && !args_complete) {
                        if (needs_closing_quote) {
                            current_tool->arguments += "\"";
                            needs_closing_quote = false;
                        }
                        current_tool->arguments += "}";
                    }
                    break;
                default:
                    break;
            }
        };
    };
}

// FunctionGemma mapper: similar to constructed but with different string handling
// Format: <start_function_call>call:name{key:<escape>value<escape>,key2:123}<end_function_call>
common_chat_peg_mapper common_chat_peg_function_gemma_mapper() {
    return [](common_chat_msg & result) -> common_chat_peg_map_func {
        auto base = common_chat_peg_base_mapper()(result);
        common_chat_tool_call * current_tool = nullptr;
        int arg_count = 0;

        return [&result, base, current_tool, arg_count](const common_peg_ast_node & node) mutable {
            base(node);

            switch (static_cast<Tag>(node.tag_id)) {
                case Tag::TOOL_OPEN:
                    result.tool_calls.emplace_back();
                    current_tool = &result.tool_calls.back();
                    arg_count = 0;
                    break;
                case Tag::TOOL_NAME:
                    if (current_tool) {
                        current_tool->name = std::string(trim_trailing_space(node.text));
                        current_tool->arguments = "{";
                    }
                    break;
                case Tag::TOOL_ARG_NAME:
                    if (current_tool) {
                        if (arg_count > 0) {
                            current_tool->arguments += ",";
                        }
                        current_tool->arguments += json(trim_trailing_space(node.text)).dump() + ":";
                        ++arg_count;
                    }
                    break;
                case Tag::TOOL_ARG_STRING_VALUE:
                    if (current_tool) {
                        // FunctionGemma values are always strings (wrapped in <escape> tags)
                        std::string value = std::string(trim_trailing_space(node.text));
                        current_tool->arguments += json(value).dump();
                    }
                    break;
                case Tag::TOOL_ARG_JSON_VALUE:
                    if (current_tool) {
                        // Raw JSON value (number, boolean, null, etc.)
                        current_tool->arguments += std::string(trim_trailing_space(node.text));
                    }
                    break;
                case Tag::TOOL_CLOSE:
                    if (current_tool) {
                        current_tool->arguments += "}";
                    }
                    break;
                default:
                    break;
            }
        };
    };
}

// Short form mapper: handles {"function_name": {"arg1": value1}} format (used by Apertus)
// The entire JSON array is captured in TOOL_ARGS, and we parse it to extract individual tool calls
common_chat_peg_mapper common_chat_peg_short_form_mapper() {
    return [](common_chat_msg & result) -> common_chat_peg_map_func {
        auto base = common_chat_peg_base_mapper()(result);

        return [&result, base](const common_peg_ast_node & node) mutable {
            base(node);

            switch (static_cast<Tag>(node.tag_id)) {
                case Tag::TOOL_ARGS: {
                    // Parse the JSON array - format is [{"func_name": {...}}, ...]
                    try {
                        auto arr = json::parse(node.text);
                        if (!arr.is_array()) {
                            break;
                        }
                        for (const auto & item : arr) {
                            if (!item.is_object() || item.size() != 1) {
                                continue;
                            }
                            // The key is the function name, the value is the arguments
                            auto it = item.begin();
                            std::string name = it.key();
                            const json & args = it.value();

                            result.tool_calls.emplace_back();
                            auto & tool = result.tool_calls.back();
                            tool.name = name;
                            if (args.is_object()) {
                                tool.arguments = args.dump();
                            } else if (args.is_string()) {
                                tool.arguments = args.get<std::string>();
                            } else if (!args.is_null()) {
                                tool.arguments = args.dump();
                            } else {
                                tool.arguments = "{}";
                            }
                        }
                    } catch (...) {
                        // JSON parse error - ignore
                    }
                    break;
                }
                default:
                    break;
            }
        };
    };
}

// Generic mapper: handles {"tool_call": {...}}, {"tool_calls": [...]}, or {"response": "..."} format
// The entire JSON is captured in TOOL_ARGS or CONTENT
common_chat_peg_mapper common_chat_peg_generic_mapper() {
    return [](common_chat_msg & result) -> common_chat_peg_map_func {
        return [&result](const common_peg_ast_node & node) mutable {
            switch (static_cast<Tag>(node.tag_id)) {
                case Tag::TOOL_ARGS: {
                    try {
                        auto data = json::parse(node.text);
                        if (data.contains("tool_calls") && data.at("tool_calls").is_array()) {
                            for (const auto & tc : data.at("tool_calls")) {
                                result.tool_calls.emplace_back();
                                auto & tool = result.tool_calls.back();
                                if (tc.contains("name")) {
                                    tool.name = tc.at("name").get<std::string>();
                                }
                                if (tc.contains("id")) {
                                    tool.id = tc.at("id").get<std::string>();
                                }
                                if (tc.contains("arguments")) {
                                    const auto & args = tc.at("arguments");
                                    tool.arguments = args.is_string() ? args.get<std::string>() : args.dump();
                                } else {
                                    tool.arguments = "{}";
                                }
                            }
                        } else if (data.contains("tool_call") && data.at("tool_call").is_object()) {
                            const auto & tc = data.at("tool_call");
                            result.tool_calls.emplace_back();
                            auto & tool = result.tool_calls.back();
                            if (tc.contains("name")) {
                                tool.name = tc.at("name").get<std::string>();
                            }
                            if (tc.contains("id")) {
                                tool.id = tc.at("id").get<std::string>();
                            }
                            if (tc.contains("arguments")) {
                                const auto & args = tc.at("arguments");
                                tool.arguments = args.is_string() ? args.get<std::string>() : args.dump();
                            } else {
                                tool.arguments = "{}";
                            }
                        } else if (data.contains("response")) {
                            const auto & resp = data.at("response");
                            result.content = resp.is_string() ? resp.get<std::string>() : resp.dump();
                        }
                    } catch (...) {
                        // JSON parse error - ignore
                    }
                    break;
                }
                case Tag::CONTENT: {
                    try {
                        auto data = json::parse(node.text);
                        if (data.contains("response")) {
                            const auto & resp = data.at("response");
                            result.content = resp.is_string() ? resp.get<std::string>() : resp.dump();
                        }
                    } catch (...) {
                        // JSON parse error - ignore
                    }
                    break;
                }
                default:
                    break;
            }
        };
    };
}

// OpenAI-style array mapper: handles [{"name": "func", "arguments": {...}, "id": "..."}] format
// Used by Mistral Nemo, Magistral, and similar formats
common_chat_peg_mapper common_chat_peg_oai_array_mapper() {
    return [](common_chat_msg & result) -> common_chat_peg_map_func {
        auto base = common_chat_peg_base_mapper()(result);

        return [&result, base](const common_peg_ast_node & node) mutable {
            base(node);

            switch (static_cast<Tag>(node.tag_id)) {
                case Tag::TOOL_ARGS: {
                    try {
                        auto arr = json::parse(node.text);
                        if (!arr.is_array()) {
                            break;
                        }
                        for (const auto & item : arr) {
                            if (!item.is_object()) {
                                continue;
                            }

                            result.tool_calls.emplace_back();
                            auto & tool = result.tool_calls.back();

                            if (item.contains("name")) {
                                tool.name = item.at("name").get<std::string>();
                            }
                            if (item.contains("id")) {
                                const auto & id = item.at("id");
                                tool.id = id.is_string() ? id.get<std::string>() : std::to_string(id.get<int>());
                            }
                            if (item.contains("arguments")) {
                                const auto & args = item.at("arguments");
                                if (args.is_object()) {
                                    tool.arguments = args.dump();
                                } else if (args.is_string()) {
                                    tool.arguments = args.get<std::string>();
                                } else if (!args.is_null()) {
                                    tool.arguments = args.dump();
                                } else {
                                    tool.arguments = "{}";
                                }
                            } else {
                                tool.arguments = "{}";
                            }
                        }
                    } catch (...) {
                        // JSON parse error - ignore
                    }
                    break;
                }
                default:
                    break;
            }
        };
    };
}

// Command R7B mapper: handles [{"tool_call_id": "0", "tool_name": "func", "parameters": {...}}] format
// The entire JSON array is captured in TOOL_ARGS, and we parse it to extract individual tool calls
common_chat_peg_mapper common_chat_peg_command_r7b_mapper() {
    return [](common_chat_msg & result) -> common_chat_peg_map_func {
        auto base = common_chat_peg_base_mapper()(result);

        return [&result, base](const common_peg_ast_node & node) mutable {
            base(node);

            switch (static_cast<Tag>(node.tag_id)) {
                case Tag::TOOL_ARGS: {
                    // Parse the JSON array - format is [{"tool_call_id": "0", "tool_name": "func", "parameters": {...}}, ...]
                    try {
                        auto arr = json::parse(node.text);
                        if (!arr.is_array()) {
                            break;
                        }
                        for (const auto & item : arr) {
                            if (!item.is_object()) {
                                continue;
                            }

                            result.tool_calls.emplace_back();
                            auto & tool = result.tool_calls.back();

                            if (item.contains("tool_name")) {
                                tool.name = item.at("tool_name").get<std::string>();
                            }
                            if (item.contains("tool_call_id")) {
                                const auto & id = item.at("tool_call_id");
                                // Can be string or number
                                tool.id = id.is_string() ? id.get<std::string>() : std::to_string(id.get<int>());
                            }
                            if (item.contains("parameters")) {
                                const auto & params = item.at("parameters");
                                if (params.is_object()) {
                                    tool.arguments = params.dump();
                                } else if (params.is_string()) {
                                    tool.arguments = params.get<std::string>();
                                } else if (!params.is_null()) {
                                    tool.arguments = params.dump();
                                } else {
                                    tool.arguments = "{}";
                                }
                            } else {
                                tool.arguments = "{}";
                            }
                        }
                    } catch (...) {
                        // JSON parse error - ignore
                    }
                    break;
                }
                default:
                    break;
            }
        };
    };
}
