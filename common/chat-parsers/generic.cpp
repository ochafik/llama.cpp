// Generic tool call format (fallback)
// Format: JSON with tool_call/tool_calls or response field
// Single: {"tool_call": {"name": "func", "arguments": {...}}}
// Multiple: {"tool_calls": [{"name": "func", "arguments": {...}}]}
// Response: {"response": "..."}

#include "chat-parsers-internal.h"
#include "chat.h"

common_chat_params common_chat_params_init_generic_peg(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    // Build PEG parser for generic JSON format
    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty() && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE;
    
    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;

        // The generic format uses JSON with specific structure
        // {"tool_calls": [...]} or {"response": "..."}
        if (has_tools) {
            static const json id_schema {
                {"type", "string"},
                {"minLength", 4},
            };
            // Tool call: <|tool_call_start|> + JSON array with schema validation + <|tool_call_end|>
            auto tool_calls = p.trigger_rule("tool-call-root", 
                build_json_tool_calls_peg_parser(p, inputs, p.literal("["), p.literal(","), p.literal("]"), "id", id_schema));

            if (inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED) {
                return "{" << p.literal("\"tool_calls\"") << ":" << tool_calls << "}";
            }

            return "{" << (p.choice()
                | (p.literal("\"tool_calls\"") << ":" << tool_calls)
                | (p.literal("\"response\"") << ":" << p.schema(p.json(), "response-format", inputs.json_schema.is_null() ? json {{"type", "string"}} : inputs.json_schema))
            ) << "}";
        }

        // json_schema without tools - parse directly without {response: ...} wrapper
        if (!inputs.json_schema.is_null()) {
            return p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema));
        }

        // No tools and no json_schema - just capture all content
        return p.tag(Tag::CONTENT, p.rest());
    });

    // Only add JSON format system message when tools are involved
    if (has_tools) {
        auto tweaked_messages = common_chat_template::add_system(
            inputs.messages,
            "Respond in JSON format, either with `tool_call` (a request to call tools) or with `response` reply to the user's request");
        data.prompt = apply(tmpl, inputs, /* messages_override= */ tweaked_messages);
    } else {
        data.prompt = apply(tmpl, inputs);
    }
    data.format = COMMON_CHAT_FORMAT_GENERIC;
    common_chat_build_peg_grammar(inputs, parser, data);

    return data;
}
