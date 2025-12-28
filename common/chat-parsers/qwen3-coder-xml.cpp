// Qwen3 Coder XML tool call format
// Format: <tool_call><function=name><parameter=key>value</parameter></function></tool_call>

#include "chat-parsers-internal.h"
#include <optional>

common_chat_params common_chat_params_init_qwen3_coder_xml_peg(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    data.prompt = apply(tmpl, inputs);

    data.preserved_tokens = {
        "<tool_call>",
        "</tool_call>",
        "<function=",
        "</function>",
        "<parameter=",
        "</parameter>",
    };

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto include_grammar = true;

    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;

        const auto consume_end_block = [&]() {
            auto optional_end = p.optional(p.choice({
                p.literal("<|im_end|>"),
                p.literal("<|endoftext|>")
            }));
            return p.optional(p.literal("\n")) + optional_end + p.optional(p.literal("\n"));
        };

        const auto content_until = [&](const std::string & marker, bool allow_inline) {
            std::vector<std::string> delimiters = {
                std::string("\r\n") + marker,
                std::string("\n") + marker,
            };
            if (allow_inline) {
                delimiters.push_back(marker);
            }
            return p.tag(Tag::CONTENT, p.until_one_of(delimiters));
        };

        const auto content_before_tool = p.optional(p.rule("qwen-tool-prefix",
            p.tag(Tag::CONTENT, p.until("<tool_call>"))
            + p.peek(p.literal("<tool_call>"))
        ));

        // Response format parser
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            return p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema))
                << consume_end_block();
        }

        // Tool call parser
        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            if (inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED) {
                data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<tool_call>"});
            }

            auto tool_call_start = p.space() + "<tool_call>\n<function=";
            auto tool_call_name_params_sep = ">" + p.space();
            auto tool_call_end = "</function>" + p.space() + "</tool_call>";
            auto param_start = p.literal("<parameter=");
            auto param_name_value_sep = ">" + p.space();
            auto param_end = "\n</parameter>\n";

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

            if (inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED) {
                return tool_calls + consume_end_block();
            }
            return p.optional(content_before_tool) + tool_calls + consume_end_block();
        }

        // Content only parser
        include_grammar = false;
        return p.choice({
            content_until("<|im_end|>", /* allow_inline = */ true) << consume_end_block(),
            content_until("<|endoftext|>", /* allow_inline = */ true) << consume_end_block(),
            p.tag(Tag::CONTENT, p.rest())
        });
    });

    common_chat_build_peg_grammar(inputs, parser, data);
    data.format = COMMON_CHAT_FORMAT_PEG_CONSTRUCTED;

    return data;
}
