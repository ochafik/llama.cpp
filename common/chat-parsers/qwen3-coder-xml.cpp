// Qwen3 Coder XML tool call format
// Format: <tool_call><function=name><parameter=key>value</parameter></function></tool_call>

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_qwen3_coder_xml_peg(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_QWEN3_CODER_XML;

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

            auto tool_calls = build_generic_tool_calls_peg_parser(
                p,
                inputs,
                "<tool_call>",
                "</tool_call><tool_call>",
                "</tool_call>",
                "<function=",
                ">",
                "</function>",
                "<parameter=",
                ">",
                "</parameter>",
                /* allow_raw_string_param_value= */ true
            );

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

    return data;
}
