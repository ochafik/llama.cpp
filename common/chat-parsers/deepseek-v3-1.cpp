// DeepSeek V3.1 tool call format
// Format: <｜tool▁calls▁begin｜><｜tool▁call▁begin｜>name<｜tool▁sep｜>{"arg":"value"}<｜tool▁call▁end｜><｜tool▁calls▁end｜>
// With optional <think>...</think> reasoning blocks

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_deepseek_v3_1(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    // Pass thinking context for DeepSeek V3.1 template
    json additional_context = {
        {"thinking", inputs.enable_thinking},
    };

    auto prompt = apply(tmpl, inputs,
                       /* messages_override= */ inputs.messages,
                       /* tools_override= */ std::nullopt,
                       additional_context);
    data.prompt = prompt;

    if (string_ends_with(data.prompt, "<think>")) {
        if (!inputs.enable_thinking) {
            data.prompt += "</think>";
        } else {
            data.thinking_forced_open = true;
        }
    }

    bool has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;

    data.format = COMMON_CHAT_FORMAT_DEEPSEEK_V3_1;
    data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED && inputs.json_schema.is_null();

    data.preserved_tokens = {
        "<think>",
        "</think>",
        "<｜tool▁calls▁begin｜>",
        "<｜tool▁call▁begin｜>",
        "<｜tool▁sep｜>",
        "<｜tool▁call▁end｜>",
        "<｜tool▁calls▁end｜>",
    };

    // Build PEG parser
    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;

        // Optional thinking block
        auto reasoning = p.eps();
        if (extract_reasoning) {
            if (data.thinking_forced_open) {
                reasoning = p.tag(Tag::REASONING, p.until("</think>")) + "</think>";
            } else {
                reasoning = p.optional("<think>" + p.tag(Tag::REASONING, p.until("</think>")) + "</think>");
            }
        }

        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            auto tool_choice = p.choice();

            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");

                // Format: name<｜tool▁sep｜>{...}<｜tool▁call▁end｜>
                tool_choice |= p.rule("tool-" + name, p.tag(Tag::TOOL,
                    p.optional(p.token_tag(Tag::TOOL_OPEN, "<｜tool▁call▁begin｜>"))
                    + p.literal_tag(Tag::TOOL_NAME, name) + p.token("<｜tool▁sep｜>")
                    + p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "tool-" + name + "-args", parameters))
                    + p.token_tag(Tag::TOOL_CLOSE, "<｜tool▁call▁end｜>")
                ));
            });

            // Accept multiple variants of the tool calls begin marker
            auto tool_calls_begin = p.choice()
                | "<｜tool▁calls▁begin｜>"
                | "<｜tool_calls_begin｜>"
                | "<｜tool calls begin｜>"
                | "<｜tool\\_calls\\_begin｜>"
                | "<｜tool▁calls｜>";

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_calls = p.trigger_rule("tool-call",
                tool_calls_begin + p.repeat(tool_choice, min_calls, max_calls) + "<｜tool▁calls▁end｜>"
            );

            // Content until tool calls marker
            auto content = p.tag(Tag::CONTENT, p.until_one_of({
                "<｜tool▁calls▁begin｜>",
                "<｜tool_calls_begin｜>",
                "<｜tool calls begin｜>",
                "<｜tool\\_calls\\_begin｜>",
                "<｜tool▁calls｜>",
            }));

            return reasoning << content << tool_calls;
        }

        // Content only parser
        return reasoning << p.tag(Tag::CONTENT, p.rest());
    });

    data.parser = parser.save();

    if (has_tools) {
        // Build grammar manually for backward compatibility
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            std::vector<std::string> tool_rules;
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");
                builder.resolve_refs(parameters);
                tool_rules.push_back(builder.add_rule(name + "-call",
                    "( \"<｜tool▁call▁begin｜>\" )? \"" + name + "<｜tool▁sep｜>"
                    "\" " + builder.add_schema(name + "-args", parameters) + " "
                    "\"<｜tool▁call▁end｜>\""));
            });
            // Distill Qwen 7B & 32B models seem confused re/ syntax of their tool call opening tag,
            // so we accept common variants (then it's all constrained)
            builder.add_rule("root",
                std::string(data.thinking_forced_open ? "( \"</think>\" space )? " : "") +
                "( \"<｜tool▁calls▁begin｜>\" | \"<｜tool_calls_begin｜>\" | \"<｜tool calls begin｜>\" | \"<｜tool\\\\_calls\\\\_begin｜>\" | \"<｜tool▁calls｜>\" ) "
                "(" + string_join(tool_rules, " | ") + ")" + (inputs.parallel_tool_calls ? "*" : "") + " "
                "\"<｜tool▁calls▁end｜>\""
                " space");
        });

        data.grammar_triggers.push_back({
            COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL,
            std::string(data.thinking_forced_open ? "[\\s\\S]*?(</think>\\s*)" : "(?:<think>[\\s\\S]*?</think>\\s*)?") +
                "(<｜tool▁calls▁begin｜>|<｜tool_calls_begin｜>|<｜tool calls begin｜>|<｜tool\\\\_calls\\\\_begin｜>|<｜tool▁calls｜>)[\\s\\S]*"
        });
    }

    return data;
}
