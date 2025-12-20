// Apriel 1.5 tool call format
// Format: <tool_calls>[{"name": "func", "arguments": {...}}]</tool_calls>
// With optional <thinking>...</thinking> reasoning blocks

#include "chat-template-internal.h"

common_chat_params common_chat_params_init_apriel_1_5(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_APRIEL_1_5;

    data.preserved_tokens = {
        "<thinking>",
        "</thinking>",
        "<tool_calls>",
        "</tool_calls>",
    };

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;
    auto include_grammar = true;

    auto parser = build_chat_peg_native_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;
        auto reasoning = p.eps();
        if (inputs.enable_thinking && extract_reasoning) {
            auto reasoning_content = p.tag(Tag::REASONING, p.until("</thinking>")) + ("</thinking>" | p.end());
            reasoning = p.optional("<thinking>" + reasoning_content);
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

            return reasoning << p.tag(Tag::CONTENT, p.until("<tool_calls>")) << tool_calls;
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
            form.scope_start = "<tool_calls>[";
            form.tool_start  = "{\"name\": \"";
            form.tool_sep    = "\", \"arguments\": {";
            form.key_start   = "\"";
            form.key_val_sep = "\": ";
            form.val_end     = ", ";
            form.tool_end    = "}, ";
            form.scope_end   = "]</tool_calls>";
            form.raw_argval  = false;
            form.last_val_end = "";
            form.last_tool_end = "}";
            return form;
        })();
        build_grammar_xml_tool_call(data, inputs.tools, form);
    }

    return data;
}
