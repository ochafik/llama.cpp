#include "chat-peg-parser.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

static std::string_view trim_trailing_space(std::string_view sv) {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
        sv.remove_suffix(1);
    }
    return sv;
}

common_chat_peg_mapper common_chat_peg_base_mapper() {
    return [](common_chat_msg & result) -> common_chat_peg_map_func {
        return [&result](const common_peg_ast_node & node) {
            if (node.tag == common_chat_peg_builder::REASONING) {
                result.reasoning_content = std::string(trim_trailing_space(node.text));
            } else if (node.tag == common_chat_peg_builder::CONTENT) {
                result.content = std::string(trim_trailing_space(node.text));
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

            if (node.tag == common_chat_peg_native_builder::TOOL_OPEN) {
                result.tool_calls.emplace_back();
                current_tool = &result.tool_calls.back();
            } else if (node.tag == common_chat_peg_native_builder::TOOL_ID && current_tool) {
                current_tool->id = std::string(trim_trailing_space(node.text));
            } else if (node.tag == common_chat_peg_native_builder::TOOL_NAME && current_tool) {
                current_tool->name = std::string(trim_trailing_space(node.text));
            } else if (node.tag == common_chat_peg_native_builder::TOOL_ARGS && current_tool) {
                current_tool->arguments = std::string(trim_trailing_space(node.text));
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

        return [&result, base, current_tool, arg_count, needs_closing_quote](const common_peg_ast_node & node) mutable {
            base(node);

            if (node.tag == common_chat_peg_constructed_builder::TOOL_OPEN) {
                result.tool_calls.emplace_back();
                current_tool = &result.tool_calls.back();
                arg_count = 0;
            } else if (node.tag == common_chat_peg_constructed_builder::TOOL_NAME && current_tool) {
                current_tool->name = std::string(node.text);
                current_tool->arguments = "{";
            } else if (node.tag == common_chat_peg_constructed_builder::TOOL_ARG_OPEN) {
                needs_closing_quote = false;
            } else if (node.tag == common_chat_peg_constructed_builder::TOOL_ARG_NAME && current_tool) {
                if (arg_count > 0) {
                    current_tool->arguments += ",";
                }
                current_tool->arguments += json(trim_trailing_space(node.text)).dump() + ":";
                ++arg_count;
            } else if (node.tag == common_chat_peg_constructed_builder::TOOL_ARG_STRING_VALUE && current_tool) {
                // Serialize to JSON, but exclude the end quote
                std::string dumped = json(node.text).dump();
                current_tool->arguments += dumped.substr(0, dumped.size() - 1);
                needs_closing_quote = true;
            } else if (node.tag == common_chat_peg_constructed_builder::TOOL_ARG_CLOSE && current_tool) {
                if (needs_closing_quote) {
                    current_tool->arguments += "\"";
                }
            } else if (node.tag == common_chat_peg_constructed_builder::TOOL_ARG_JSON_VALUE && current_tool) {
                current_tool->arguments += std::string(trim_trailing_space(node.text));
            } else if (node.tag == common_chat_peg_constructed_builder::TOOL_CLOSE && current_tool) {
                current_tool->arguments += "}";
            }
        };
    };
}
