// Command R7B tool call format
// Format: <|START_THINKING|>...<|END_THINKING|><|START_ACTION|>[{"tool_call_id":"1","tool_name":"func","parameters":{}}]<|END_ACTION|>

#include "chat-template-internal.h"

common_chat_params common_chat_params_init_command_r7b(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    auto adjusted_messages = json::array();
    for (const auto & msg : inputs.messages) {
        auto has_reasoning_content = msg.contains("reasoning_content") && msg.at("reasoning_content").is_string();
        auto has_tool_calls = msg.contains("tool_calls") && msg.at("tool_calls").is_array();
        if (has_reasoning_content && has_tool_calls) {
            auto adjusted_message = msg;
            adjusted_message["tool_plan"] = msg.at("reasoning_content");
            adjusted_message.erase("reasoning_content");
            adjusted_messages.push_back(adjusted_message);
        } else {
            adjusted_messages.push_back(msg);
        }
    }
    data.prompt = apply(tmpl, inputs, /* messages_override= */ adjusted_messages);

    if (string_ends_with(data.prompt, "<|START_THINKING|>")) {
        if (!inputs.enable_thinking) {
            data.prompt += "<|END_THINKING|>";
        } else {
            data.thinking_forced_open = true;
        }
    } else if (!inputs.enable_thinking && string_ends_with(data.prompt, "<|CHATBOT_TOKEN|>")) {
        data.prompt += "<|START_THINKING|><|END_THINKING|>";
    }

    bool has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;

    data.format = COMMON_CHAT_FORMAT_COMMAND_R7B;
    data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;

    data.preserved_tokens = {
        "<|START_ACTION|>",
        "<|END_ACTION|>",
        "<|START_RESPONSE|>",
        "<|END_RESPONSE|>",
        "<|START_THINKING|>",
        "<|END_THINKING|>",
    };

    // Build PEG parser
    auto parser = build_chat_peg_native_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;

        // Optional thinking block
        auto reasoning = p.eps();
        if (extract_reasoning) {
            if (data.thinking_forced_open) {
                // Thinking was already started by template
                reasoning = p.tag(Tag::REASONING, p.until("<|END_THINKING|>")) + "<|END_THINKING|>";
            } else {
                reasoning = p.optional("<|START_THINKING|>" + p.tag(Tag::REASONING, p.until("<|END_THINKING|>")) + "<|END_THINKING|>");
            }
        }

        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            // Tool call: <|START_ACTION|>[...json array...]<|END_ACTION|>
            auto tool_call = p.tag(Tag::TOOL,
                p.token_tag(Tag::TOOL_OPEN, "<|START_ACTION|>")
                + p.tag(Tag::TOOL_ARGS, p.json())  // JSON array with tool calls
                + p.token_tag(Tag::TOOL_CLOSE, "<|END_ACTION|>")
            );

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_calls = p.trigger_rule("tool-call", p.repeat(tool_call, min_calls, max_calls));

            // Content until we see the action marker
            auto content = p.tag(Tag::CONTENT, p.until("<|START_ACTION|>"));

            return reasoning << content << tool_calls;
        }

        // Content only parser
        return reasoning << p.tag(Tag::CONTENT, p.rest());
    });

    data.parser = parser.save();

    if (has_tools) {
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            auto schemas = json::array();
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                schemas.push_back({
                    {"type", "object"},
                    {"properties", {
                        {"tool_call_id", {
                            {"type", "string"},
                            // Command-R's template expects an integer string.
                            {"pattern", "^[0-9]{1,10}$"},
                        }},
                        {"tool_name", {
                            {"type", "string"},
                            {"const", function.at("name")},
                        }},
                        {"parameters", function.at("parameters")},
                    }},
                    {"required", json::array({"tool_call_id", "tool_name", "parameters"})},
                });
            });
            auto schema = json {
                {"type", "array"},
                {"items", schemas.size() == 1 ? schemas[0] : json {{"anyOf", schemas}}},
                {"minItems", 1},
            };
            if (!inputs.parallel_tool_calls) {
                schema["maxItems"] = 1;
            }
            builder.add_rule("root",
                std::string(data.thinking_forced_open ? "( \"<|END_THINKING|>\" space )? " : "") +
                "\"<|START_ACTION|>\" " + builder.add_schema("tool_calls", schema) + " \"<|END_ACTION|>\"");
        });

        data.grammar_triggers.push_back({
            COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL,
            std::string(data.thinking_forced_open ? "[\\s\\S]*?(<\\|END_THINKING\\|>\\s*)" : "(?:<\\|START_THINKING\\|>[\\s\\S]*?<\\|END_THINKING\\|>\\s*)?") +
                "(<\\|START_ACTION\\|>)[\\s\\S]*"
        });
    }

    return data;
}
