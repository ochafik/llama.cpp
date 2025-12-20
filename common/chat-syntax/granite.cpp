// Granite tool call format
// Format: <|tool_call|>[{"name": "func", "arguments": {...}}]
// With optional <think>...</think> and <response>...</response> tags

#include "chat-template-internal.h"

common_chat_params common_chat_params_init_granite(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    // Pass thinking context for Granite template
    json additional_context = {
        {"thinking", inputs.enable_thinking},
    };

    data.prompt = apply(tmpl, inputs, /* messages_override= */ std::nullopt, /* tools_override= */ std::nullopt, additional_context);
    data.format = COMMON_CHAT_FORMAT_GRANITE;

    if (string_ends_with(data.prompt, "<think>\n") || string_ends_with(data.prompt, "<think>")) {
        if (!inputs.enable_thinking) {
            data.prompt += "</think>";
        } else {
            data.thinking_forced_open = true;
        }
    }

    data.preserved_tokens = {
        "<think>",
        "</think>",
        "<response>",
        "</response>",
        "<|tool_call|>",
    };

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;
    auto include_grammar = true;

    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;

        auto reasoning = p.eps();
        if (inputs.enable_thinking && extract_reasoning) {
            auto reasoning_content = p.tag(Tag::REASONING, p.until("</think>")) + ("</think>" | p.end());
            if (data.thinking_forced_open) {
                reasoning = reasoning_content;
            } else {
                reasoning = p.optional("<think>" + reasoning_content);
            }
        }

        // Response format parser
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            return reasoning << p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema));
        }

        // Tool call parser
        // Format: <|tool_call|>[{"name": "func", "arguments": {...}}]
        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            auto tool_call = p.tag(Tag::TOOL,
                p.token_tag(Tag::TOOL_OPEN, "<|tool_call|>")
                + p.tag(Tag::TOOL_ARGS, p.json())
            );

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_calls = p.trigger_rule("tool-call", p.repeat(tool_call, min_calls, max_calls));

            // Content may be wrapped in <response>...</response>
            auto content = p.tag(Tag::CONTENT, p.until("<|tool_call|>"));

            return reasoning << content << tool_calls;
        }

        // Content only parser
        include_grammar = false;
        return reasoning << p.tag(Tag::CONTENT, p.rest());
    });

    data.parser = parser.save();

    if (include_grammar) {
        data.grammar_lazy = has_tools && inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_AUTO;
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            std::vector<std::string> tool_rules;
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");
                builder.resolve_refs(parameters);
                tool_rules.push_back(builder.add_rule(name + "-call", builder.add_schema(name + "-args", {
                    {"type", "object"},
                    {"properties", {
                        {"name", {{"const", name}}},
                        {"arguments", parameters},
                    }},
                    {"required", json::array({"name", "arguments"})},
                })));
            });

            auto tool_call = builder.add_rule("tool_call", string_join(tool_rules, " | "));
            auto tool_list = builder.add_rule("tool_list", "\"[\" space " + tool_call + " (\",\" space " + tool_call + ")* space \"]\"");

            if (data.thinking_forced_open) {
                builder.add_rule("root", "\"</think>\" space \"<response>\" space [^<]* \"</response>\" space \"<|tool_call|>\" space " + tool_list);
            } else {
                builder.add_rule("root", "\"<|tool_call|>\" space " + tool_list);
            }

            data.grammar_triggers.push_back({
                COMMON_GRAMMAR_TRIGGER_TYPE_WORD,
                "<|tool_call|>"
            });
        });
    }

    return data;
}
