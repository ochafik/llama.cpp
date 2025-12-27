// Magistral tool call format
// Format: [THINK]...[/THINK][TOOL_CALLS][{"name":"func","arguments":{},"id":"abc123def"}]

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_magistral_peg(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_MAGISTRAL;

    data.preserved_tokens = {
        "[THINK]",
        "[/THINK]",
    };

    bool has_tools = inputs.tools.is_array() && !inputs.tools.empty() && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE;
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;

    // Build the PEG parser
    bool require_tools = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED;
    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;

        // Optional reasoning block
        auto reasoning = extract_reasoning
            ? p.optional("[THINK]" + p.tag(Tag::REASONING, p.until("[/THINK]")) + "[/THINK]")
            : p.eps();

        // Response format parser (json_schema support)
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            return reasoning << p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema));
        }

        if (has_tools) {
            if (inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED) {
                data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "[TOOL_CALLS]"});
                data.preserved_tokens.push_back("[TOOL_CALLS]");
            }

            // Tool call parser: content followed by [TOOL_CALLS] and JSON array
            auto tool_calls = p.trigger_rule("tool-call-root",
                build_json_args_peg_parser(p, inputs, json {
                    {"type", "string"},
                    {"pattern", "^[a-zA-Z0-9]{9}$"},  // Enforce ID format (exactly 9 alphanumeric)
                }, "[TOOL_CALLS][", ",", "]"));

            if (require_tools) {
                return reasoning << tool_calls;
            }
            return reasoning << p.tag(Tag::CONTENT, p.until("[TOOL_CALLS]")) << tool_calls;
        }

        // Content only parser
        return reasoning << p.tag(Tag::CONTENT, p.rest());
    });

    common_chat_build_peg_grammar(inputs, parser, data);

    return data;
}
