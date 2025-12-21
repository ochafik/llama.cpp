// Apriel 1.5 tool call format
// Format: <tool_calls>[{"name": "func", "arguments": {...}}]</tool_calls>
// With optional <thinking>...</thinking> reasoning blocks

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_apriel_1_5(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_APRIEL_1_5;

    // Handle thinking tags appropriately based on inputs.enable_thinking
    if (string_ends_with(data.prompt, "<thinking>\n") || string_ends_with(data.prompt, "<thinking>")) {
        if (!inputs.enable_thinking) {
            data.prompt += "</thinking>";
        } else {
            data.thinking_forced_open = true;
        }
    }

    data.preserved_tokens = {
        "<thinking>",
        "</thinking>",
        "<tool_calls>",
        "</tool_calls>",
    };

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;
    auto include_grammar = true;

    bool require_tools = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED;
    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;
        auto reasoning = p.eps();
        if (inputs.enable_thinking && extract_reasoning) {
            auto reasoning_content = p.tag(Tag::REASONING, p.until("</thinking>")) + ("</thinking>" | p.end());
            if (data.thinking_forced_open) {
                reasoning = reasoning_content;
            } else {
                reasoning = p.optional("<thinking>" + reasoning_content);
            }
        }

        // Response format parser
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            return reasoning << p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema));
        }

        // Tool call parser
        // Format: <tool_calls>[{"name": "func", "arguments": {...}}]</tool_calls>
        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            auto tool_call = p.tag(Tag::TOOL,
                p.token_tag(Tag::TOOL_OPEN, "<tool_calls>")
                + p.tag(Tag::TOOL_ARGS, p.json())
                + p.token_tag(Tag::TOOL_CLOSE, "</tool_calls>")
            );

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_calls = p.trigger_rule("tool-call", p.repeat(tool_call, min_calls, max_calls));

            if (require_tools) {
                return reasoning << tool_calls;
            }
            return reasoning << p.tag(Tag::CONTENT, p.until("<tool_calls>")) << tool_calls;
        }

        // Content only parser
        include_grammar = false;
        return reasoning << p.tag(Tag::CONTENT, p.rest());
    });

    data.parser = parser.save();

    if (include_grammar) {
        data.grammar_lazy = has_tools && inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_AUTO;

        // Build grammar from PEG parser
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            foreach_function(inputs.tools, [&](const json & tool) {
                auto schema = tool.at("function").at("parameters");
                builder.resolve_refs(schema);
            });
            parser.build_grammar(builder, data.grammar_lazy);
        });

        if (data.grammar_lazy) {
            data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<tool_calls>"});
        } else {
            data.grammar_triggers.clear();
        }
    }

    return data;
}
