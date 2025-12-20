// Apertus tool call format
// Format: <|tools_prefix|>[{"func_name": {"arg1": value1}}]<|tools_suffix|>
// With optional <|inner_prefix|>...<|inner_suffix|> reasoning blocks

#include "chat-template-internal.h"

common_chat_params common_chat_params_init_apertus(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_APERTUS;

    // Handle thinking tags appropriately based on inputs.enable_thinking
    if (string_ends_with(data.prompt, "<|inner_prefix|>")) {
        if (!inputs.enable_thinking) {
            data.prompt += "<|inner_suffix|>";
        } else {
            data.thinking_forced_open = true;
        }
    }

    data.preserved_tokens = {
        "<|system_start|>",
        "<|system_end|>",
        "<|developer_start|>",
        "<|developer_end|>",
        "<|user_start|>",
        "<|user_end|>",
        "<|assistant_start|>",
        "<|assistant_end|>",
        "<|inner_prefix|>",
        "<|inner_suffix|>",
        "<|tools_prefix|>",
        "<|tools_suffix|>",
    };

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;
    auto include_grammar = true;

    auto parser = build_chat_peg_native_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;
        auto reasoning = p.eps();
        if (inputs.enable_thinking && extract_reasoning) {
            auto reasoning_content = p.tag(Tag::REASONING, p.until("<|inner_suffix|>")) + ("<|inner_suffix|>" | p.end());
            if (data.thinking_forced_open) {
                reasoning = reasoning_content;
            }
        }

        // Response format parser
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            return reasoning << p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema));
        }

        // Tool call parser - short form JSON array format
        // Format: <|tools_prefix|>[{"func_name": {...}}]<|tools_suffix|>
        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            // Tool call: <|tools_prefix|> + JSON array + <|tools_suffix|>
            auto tool_call = p.tag(Tag::TOOL,
                p.token_tag(Tag::TOOL_OPEN, "<|tools_prefix|>")
                + p.tag(Tag::TOOL_ARGS, p.json())
                + p.token_tag(Tag::TOOL_CLOSE, "<|tools_suffix|>")
            );

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_calls = p.trigger_rule("tool-call", p.repeat(tool_call, min_calls, max_calls));

            return reasoning << p.tag(Tag::CONTENT, p.until("<|tools_prefix|>")) << tool_calls;
        }

        // Content only parser
        include_grammar = false;
        return reasoning << p.tag(Tag::CONTENT, p.rest());
    });

    data.parser = parser.save();

    if (include_grammar) {
        data.grammar_lazy = has_tools && inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_AUTO;

        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            auto schemas = json::array();
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                // Apertus uses short form: {"func_name": {"arg1": value1}}
                schemas.push_back({
                    {"type", "object"},
                    {"properties", {
                        {function.at("name"), function.at("parameters")}
                    }},
                    {"required", json::array({function.at("name")})},
                });
            });
            auto schema = json{
                {"type", "array"},
                {"items", schemas.size() == 1 ? schemas[0] : json{{"anyOf", schemas}}},
                {"minItems", 1},
            };
            if (!inputs.parallel_tool_calls) {
                schema["maxItems"] = 1;
            }
            builder.add_rule("root",
                std::string(data.thinking_forced_open ? "( \"<|inner_suffix|>\" space )? " : "") +
                "\"<|tools_prefix|>\" " + builder.add_schema("tool_calls", schema) + " \"<|tools_suffix|>\"");
        });

        data.grammar_triggers = {{COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL,
            // If thinking_forced_open, then we capture the <|inner_suffix|> tag in the grammar
            std::string(data.thinking_forced_open ?
                "[\\s\\S]*?(<\\|inner_suffix\\|>\\s*)" :
                "(?:<\\|inner_prefix\\|>[\\s\\S]*?<\\|inner_suffix\\|>\\s*)?") +
            "(<\\|tools_prefix\\|>)[\\s\\S]*"}};
    }

    return data;
}
