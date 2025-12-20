// Hermes 2 Pro tool call format
// Formats:
// - <tool_call>{"name":"func","arguments":{}}</tool_call>
// - <function=name>{"key":"value"}</function>
// - <function name="name">{"key":"value"}</function>
// With optional <think>...</think> reasoning blocks

#include "chat-template-internal.h"

common_chat_params common_chat_params_init_hermes_2_pro(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    json extra_context = json {
        {"enable_thinking", inputs.enable_thinking},
    };
    extra_context.update(inputs.extra_context);

    data.prompt = apply(tmpl, inputs, /* messages_override =*/ std::nullopt, /* tools_override= */ std::nullopt, extra_context);

    if (string_ends_with(data.prompt, "<think>\n")) {
        if (!extra_context["enable_thinking"]) {
            data.prompt += "</think>";
        } else {
            data.thinking_forced_open = true;
        }
    }

    bool has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;

    data.format = COMMON_CHAT_FORMAT_PEG_NATIVE;
    data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;

    data.preserved_tokens = {
        "<think>",
        "</think>",
        "<tool_call>",
        "</tool_call>",
        "<function",
        "<tools>",
        "</tools>",
        "<response>",
        "</response>",
        "<function_call>",
        "</function_call>",
        "<json>",
        "</json>",
        "<JSON>",
        "</JSON>",
        "```",
        "```json",
        "```xml",
    };

    // Build PEG parser
    auto parser = build_chat_peg_native_parser([&](auto & p) {
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

                // <tool_call>{"name":"func","arguments":{}}</tool_call>
                tool_choice |= p.rule("tool-call-" + name, p.tag(Tag::TOOL,
                    p.token_tag(Tag::TOOL_OPEN, "<tool_call>")
                    + p.space()
                    + "{" + p.space()
                    + "\"name\"" + p.space() + ":" + p.space()
                    + p.literal_tag(Tag::TOOL_NAME, "\"" + name + "\"") + p.space() + "," + p.space()
                    + "\"arguments\"" + p.space() + ":" + p.space()
                    + p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "tool-" + name + "-args", parameters))
                    + p.space() + "}"
                    + p.space()
                    + p.token_tag(Tag::TOOL_CLOSE, "</tool_call>")
                ));

                // <function=name>{...}</function>
                tool_choice |= p.rule("func-eq-" + name, p.tag(Tag::TOOL,
                    p.atomic_tag(Tag::TOOL_OPEN, "<function=" + p.literal_tag(Tag::TOOL_NAME, name) + ">")
                    + p.space()
                    + p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "func-" + name + "-args", parameters))
                    + p.space()
                    + p.token_tag(Tag::TOOL_CLOSE, "</function>")
                ));

                // <function name="name">{...}</function>
                tool_choice |= p.rule("func-name-" + name, p.tag(Tag::TOOL,
                    p.atomic_tag(Tag::TOOL_OPEN, "<function" + p.space() + "name=" + p.literal_tag(Tag::TOOL_NAME, "\"" + name + "\"") + ">")
                    + p.space()
                    + p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "funcn-" + name + "-args", parameters))
                    + p.space()
                    + p.token_tag(Tag::TOOL_CLOSE, "</function>")
                ));
            });

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_calls = p.trigger_rule("tool-call", p.repeat(tool_choice, min_calls, max_calls));

            // Content until we see tool call markers
            auto content = p.tag(Tag::CONTENT, p.until_one_of({
                "<tool_call>",
                "<function",
            }));

            return reasoning << content << tool_calls;
        }

        // Content only parser
        return reasoning << p.tag(Tag::CONTENT, p.rest());
    });

    data.parser = parser.save();

    if (has_tools) {
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            std::vector<std::string> escaped_names;
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                auto schema = function.at("parameters");
                builder.resolve_refs(schema);
                escaped_names.push_back(regex_escape(function.at("name").get<std::string>()));
            });
            parser.build_grammar(builder, data.grammar_lazy);

            // Add triggers
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");

                data.grammar_triggers.push_back({
                    COMMON_GRAMMAR_TRIGGER_TYPE_WORD,
                    "<function=" + name + ">",
                });
                data.grammar_triggers.push_back({
                    COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN,
                    "<function\\s+name\\s*=\\s*\"" + regex_escape(name) + "\"",
                });
            });

            data.grammar_triggers.push_back({
                COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL,
                std::string(data.thinking_forced_open ? "[\\s\\S]*?(</think>\\s*)" : "(?:<think>[\\s\\S]*?</think>\\s*)?") + (
                    "\\s*("
                    "(?:<tool_call>"
                    "|<function"
                    "|(?:```(?:json|xml)?\n\\s*)?(?:<function_call>|<tools>|<xml><json>|<response>)?"
                    "\\s*\\{\\s*\"name\"\\s*:\\s*\"(?:" + string_join(escaped_names, "|") + ")\""
                    ")"
                    ")[\\s\\S]*"
                ),
            });
        });
    }

    return data;
}
