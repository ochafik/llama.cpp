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

    bool has_tools = inputs.tools.is_array() && !inputs.tools.empty();

    // Build the tool calls schema for validation
    // This validates: tool names (const), parameter types, ID pattern (9 alphanumeric chars), required fields
    json tool_calls_schema = nullptr;
    if (has_tools) {
        auto schemas = json::array();
        foreach_function(inputs.tools, [&](const auto &, const auto & name, const json & parameters, const auto &) {
            schemas.push_back({
                {"type", "object"},
                {"properties", {
                    {"name", {
                        {"type", "string"},
                        {"const", name},  // Enforce exact tool name
                    }},
                    {"arguments", parameters},  // Full parameter validation
                    {"id", {
                        {"type", "string"},
                        {"pattern", "^[a-zA-Z0-9]{9}$"},  // 9-character alphanumeric ID
                    }},
                }},
                {"required", json::array({"name", "arguments", "id"})},
            });
        });

        tool_calls_schema = json{
            {"type", "array"},
            {"items", schemas.size() == 1 ? schemas[0] : json{{"anyOf", schemas}}},
            {"minItems", 1},
        };
        if (!inputs.parallel_tool_calls) {
            tool_calls_schema["maxItems"] = 1;
        }
    }

    // Build the PEG parser
    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;

        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            if (inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED) {
                data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "[TOOL_CALLS]"});
            }

            // Tool call parser: [TOOL_CALLS] followed by a JSON array of tool calls
            // The schema validates tool names, parameters, ID format, required fields, and array bounds
            auto tool_call = p.tag(Tag::TOOL,
                p.atomic_tag(Tag::TOOL_OPEN, p.literal("[TOOL_CALLS]"))
                + p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "tool-calls", tool_calls_schema))
            );

            // No repeat needed - [TOOL_CALLS] appears once with the entire array
            auto tool_calls = p.trigger_rule("tool-call-root", tool_call);

            bool require_tools = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED;
            if (require_tools) {
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
