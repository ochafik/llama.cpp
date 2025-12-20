#include "chat-peg-parser.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using Tag = common_chat_peg_tag;

static std::string_view trim_trailing_space(std::string_view sv) {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
        sv.remove_suffix(1);
    }
    return sv;
}

common_chat_peg_mapper common_chat_peg_base_mapper() {
    return [](common_chat_msg & result) -> common_chat_peg_map_func {
        return [&result](const common_peg_ast_node & node) {
            switch (static_cast<Tag>(node.tag_id)) {
                case Tag::REASONING:
                    result.reasoning_content = std::string(trim_trailing_space(node.text));
                    break;
                case Tag::CONTENT:
                    result.content = std::string(trim_trailing_space(node.text));
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

        return [&result, base, current_tool, arg_count, needs_closing_quote](const common_peg_ast_node & node) mutable {
            base(node);

            switch (static_cast<Tag>(node.tag_id)) {
                case Tag::TOOL_OPEN:
                    result.tool_calls.emplace_back();
                    current_tool = &result.tool_calls.back();
                    arg_count = 0;
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
                    }
                    break;
                case Tag::TOOL_ARG_JSON_VALUE:
                    if (current_tool) {
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
