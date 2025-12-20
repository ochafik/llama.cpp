// Kimi K2 tool call format
// Format: <|tool_calls_section_begin|><|tool_call_begin|>function_name<|tool_call_argument_begin|>{"key": value}<|tool_call_end|><|tool_calls_section_end|>
// With optional <think>...</think> reasoning blocks

#include "chat-template-internal.h"

common_chat_params common_chat_params_init_kimi_k2(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_KIMI_K2;

    data.preserved_tokens = {
        "<think>",
        "</think>",
        "<|tool_calls_section_begin|>",
        "<|tool_call_begin|>",
        "<|tool_call_argument_begin|>",
        "<|tool_call_end|>",
        "<|tool_calls_section_end|>",
        "<|im_end|>",
        "<|im_system|>",
        "<|im_middle|>",
    };

    data.additional_stops.insert(data.additional_stops.end(), {
        "<|im_end|>",
        "<|im_middle|>"
    });

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;
    auto include_grammar = true;

    auto parser = build_chat_peg_native_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;
        auto reasoning = p.eps();
        if (inputs.enable_thinking && extract_reasoning) {
            auto reasoning_content = p.tag(Tag::REASONING, p.until("</think>")) + ("</think>" | p.end());
            reasoning = p.optional("<think>" + reasoning_content);
        }

        // Response format parser
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            return reasoning << p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema));
        }

        // Tool call parser
        // Format: <|tool_call_begin|>NAME<|tool_call_argument_begin|>{...}<|tool_call_end|>
        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            // Individual tool call
            auto tool_call = p.tag(Tag::TOOL,
                p.token_tag(Tag::TOOL_OPEN, "<|tool_call_begin|>")
                + p.tag(Tag::TOOL_NAME, p.until("<|tool_call_argument_begin|>"))
                + "<|tool_call_argument_begin|>"
                + p.tag(Tag::TOOL_ARGS, p.json())
                + p.token_tag(Tag::TOOL_CLOSE, "<|tool_call_end|>")
            );

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_calls = p.trigger_rule("tool-call",
                "<|tool_calls_section_begin|>"
                + p.repeat(tool_call, min_calls, max_calls)
                + "<|tool_calls_section_end|>"
            );

            return reasoning << p.tag(Tag::CONTENT, p.until("<|tool_calls_section_begin|>")) << tool_calls;
        }

        // Content only parser
        include_grammar = false;
        return reasoning << p.tag(Tag::CONTENT, p.rest());
    });

    data.parser = parser.save();

    if (include_grammar) {
        data.grammar_lazy = has_tools && inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_AUTO;

        // Build grammar using XML tool call helper
        static const xml_tool_call_format form = ([]() {
            xml_tool_call_format form {};
            form.scope_start = "<|tool_calls_section_begin|>";
            form.tool_start  = "<|tool_call_begin|>";
            form.tool_sep    = "<|tool_call_argument_begin|>{";
            form.key_start   = "\"";
            form.key_val_sep = "\": ";
            form.val_end     = ", ";
            form.tool_end    = "}<|tool_call_end|>";
            form.scope_end   = "<|tool_calls_section_end|>";
            form.raw_argval  = false;
            form.last_val_end = "";
            return form;
        })();
        build_grammar_xml_tool_call(data, inputs.tools, form);
    }

    return data;
}
