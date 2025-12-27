// Xiaomi MiMo tool call format
// Format: <tool_call>{"name": "func", "arguments": {...}}</tool_call>

#include "chat-parsers-internal.h"
#include <optional>

common_chat_params common_chat_params_init_xiaomi_mimo_peg(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_XIAOMI_MIMO;

    data.preserved_tokens = {
        "<tool_call>",
        "</tool_call>",
    };

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty() && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE;
    auto include_grammar = true;

    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;

        // Response format parser
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            return p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema));
        }

        // Tool call parser
        // Format: <tool_call>{"name": "func", "arguments": {...}}</tool_call>
        if (has_tools) {
            if (inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED) {
                data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<tool_call>"});
            }

            auto tool_calls = p.trigger_rule("tool-call-root",
                build_json_tool_calls_peg_parser(p, inputs, 
                    p.literal("<tool_call>"),
                    p.literal("</tool_call><tool_call>"),
                    p.literal("</tool_call>")
                ));

            if (inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED) {
                return tool_calls;
            }

            // Content until <tool_call>, then consume optional newline before tools
            return p.tag(Tag::CONTENT, p.until_one_of({"<tool_call>", "\n<tool_call>"}))
                << p.optional(p.literal("\n")) << tool_calls;
        }

        // Content only parser
        include_grammar = false;
        return p.tag(Tag::CONTENT, p.rest());
    });

    common_chat_build_peg_grammar(inputs, parser, data);

    return data;
}
