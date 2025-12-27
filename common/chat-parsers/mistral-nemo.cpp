// Mistral Nemo tool call format
// Format: [TOOL_CALLS][{"name":"func","arguments":{},"id":"abc123def"}]

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_mistral_nemo_peg(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_MISTRAL_NEMO;

    data.preserved_tokens = {
        "[TOOL_CALLS]",
    };

    bool has_tools = inputs.tools.is_array() && !inputs.tools.empty() && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE;

    // Build the PEG parser
    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;

        if (has_tools) {
            if (inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED) {
                data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "[TOOL_CALLS]"});
            }

            // Tool call parser: [TOOL_CALLS] followed by a JSON array of tool calls
            auto tool_calls = p.trigger_rule("tool-call-root",
                build_json_args_peg_parser(p, inputs, json {
                    {"type", "string"},
                    {"pattern", "^[a-zA-Z0-9]{9}$"},  // Enforce ID format (exactly 9 alphanumeric)
                }, "[TOOL_CALLS][", ",", "]"));

            if (inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED) {
                return tool_calls;
            }
            return p.tag(Tag::CONTENT, p.until("[TOOL_CALLS]")) << tool_calls;
        }

        // Content only parser
        return p.tag(Tag::CONTENT, p.rest());
    });

    common_chat_build_peg_grammar(inputs, parser, data);

    return data;
}
