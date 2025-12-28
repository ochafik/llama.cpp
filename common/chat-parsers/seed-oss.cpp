// Seed OSS tool call format
// Format: <seed:tool_call><function=name><parameter=key>value</parameter></function></seed:tool_call>
// With optional <seed:think>...</seed:think> reasoning blocks

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_seed_oss_peg(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    data.prompt = apply(tmpl, inputs);

    // Handle thinking tags appropriately based on inputs.enable_thinking
    if (string_ends_with(data.prompt, "<seed:think>")) {
        if (!inputs.enable_thinking) {
            data.prompt += "</seed:think>";
        } else {
            data.thinking_forced_open = true;
        }
    }

    data.preserved_tokens = {
        "<seed:think>",
        "</seed:think>",
        "<seed:tool_call>",
        "</seed:tool_call>",
        "<function=",
        "</function>",
        "<parameter=",
        "</parameter>",
        "<seed:eos>",
    };

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;

    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;
        auto newline = p.choice({p.literal("\r\n"), p.literal("\n")});
        // Limit newlines around <seed:eos> to prevent grammar from accepting unlimited newlines
        auto eos = p.optional(p.repeat(newline, 0, 2) + p.literal("<seed:eos>") + p.repeat(newline, 0, 2));
        auto reasoning = p.eps();
        auto reasoning_block = p.literal("<seed:think>")
            + p.tag(Tag::REASONING, p.until("</seed:think>"))
            + (p.literal("</seed:think>") | p.end());
        if (extract_reasoning) {
            if (inputs.enable_thinking && data.thinking_forced_open) {
                reasoning = reasoning_block;
            } else if (inputs.enable_thinking) {
                reasoning = p.optional(reasoning_block);
            } else {
                reasoning = p.optional(reasoning_block);
            }
        } else {
            reasoning = p.optional(reasoning_block);
        }

        // Response format parser
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            return reasoning << p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema));
        }

        // Tool call parser
        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            if (inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED) {
                data.grammar_triggers = {
                    {COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<seed:tool_call>"}
                };
            }

            auto tool_call_start = p.space() + "<seed:tool_call>\n<function=";
            auto tool_call_name_params_sep = p.literal(">\n");
            auto tool_call_end = "</function>" + p.space() + "</seed:tool_call>";
            auto param_start = p.literal("<parameter=");
            auto param_name_value_sep = p.literal(">");
            auto param_end = "</parameter>\n";

            auto tool_call = p.choice();
            foreach_function(inputs.tools, [&](const auto &, const auto & name, const json & parameters, const auto & schema_info) {
                auto args = p.sequence();
                foreach_parameter(p, parameters, [&](const std::string & param_name, const common_peg_parser & param_p, const json & param_schema, ParameterType param_type) {
                    auto arg = p.rule("tool-" + name + "-arg-" + param_name,
                        p.tag(Tag::TOOL_ARG_OPEN, param_start)
                        + p.tag(Tag::TOOL_ARG_NAME, param_p)
                        + param_name_value_sep
                        + p.schema_or_raw_string_until("tool-" + name + "-arg-" + param_name + "-schema", param_schema, param_end,
                            schema_info, Tag::TOOL_ARG_STRING_VALUE, Tag::TOOL_ARG_JSON_VALUE, true)
                        + p.literal_tag(Tag::TOOL_ARG_CLOSE, param_end));
                    switch (param_type) {
                        case ParameterType::Required:
                            args += arg;
                            break;
                        case ParameterType::Optional:
                            args += p.optional(arg);
                            break;
                        case ParameterType::Additional:
                            args += p.repeat(arg, 0, -1);
                            break;
                        default:
                            throw std::runtime_error("Unhandled param type");
                    }
                });

                tool_call |= p.rule("tool-" + name,
                    p.tag(Tag::TOOL_OPEN, tool_call_start)
                    + p.literal_tag(Tag::TOOL_NAME, name)
                    + tool_call_name_params_sep
                    + args
                    + p.tag(Tag::TOOL_CLOSE, tool_call_end));
            });

            auto tool_calls = tool_call + p.repeat(tool_call, 0, inputs.parallel_tool_calls ? -1 : 0);

            auto stop_before = std::vector<std::string> {
                "\r\n\r\n<seed:tool_call>", "\n\n<seed:tool_call>",
                "\r\n<seed:tool_call>", "\n<seed:tool_call>", "<seed:tool_call>",
                "\r\n\r\n<seed:toolcall>", "\n\n<seed:toolcall>",
                "\r\n<seed:toolcall>", "\n<seed:toolcall>", "<seed:toolcall>",
            };
            auto content_before = p.optional(p.tag(Tag::CONTENT, p.until_one_of(stop_before)));
            // After tool calls, only allow limited trailing whitespace (not arbitrary content)
            // to prevent the grammar from allowing unlimited newlines
            auto post_tool_gap = p.repeat(newline, 0, 2);
            auto pre_calls_gap = p.repeat(newline, 0, -1);
            if (inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED) {
                return reasoning << pre_calls_gap << tool_calls << post_tool_gap << eos;
            }
            return reasoning << content_before << pre_calls_gap << tool_calls << post_tool_gap << eos;
        }

        // Content only parser
        auto content_tail = p.optional(p.tag(Tag::CONTENT, p.until_one_of({
            "\r\n\r\n<seed:eos>", "\n\n<seed:eos>",
            "\r\n<seed:eos>", "\n<seed:eos>", "<seed:eos>"
        })));
        // Limit trailing newlines before eos to prevent grammar from accepting unlimited newlines
        auto pre_eos_gap = p.repeat(newline, 0, 2);
        return reasoning << content_tail << pre_eos_gap << eos;
    });

    common_chat_build_peg_grammar(inputs, parser, data);
    data.format = COMMON_CHAT_FORMAT_PEG_CONSTRUCTED;

    return data;
}
