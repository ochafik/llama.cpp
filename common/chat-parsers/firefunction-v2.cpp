// Firefunction V2 tool call format
// Format: functools[{"name":"func","arguments":{}}]

#include "chat-parsers-internal.h"
common_chat_params common_chat_params_init_firefunction_v2_peg(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    const std::optional<json> tools_override = json();
    const std::optional<json> additional_context = json {
        {"datetime", format_time(inputs.now, "%b %d %Y %H:%M:%S GMT")},
        {"functions", json(inputs.tools.empty() ? "" : inputs.tools.dump(2))},
    };
    data.preserved_tokens = {
        " functools[",
    };
    data.prompt = apply(tmpl, inputs, /* messages_override =*/ std::nullopt, tools_override, additional_context);

    bool has_tools = inputs.tools.is_array() && !inputs.tools.empty();

    // Build schema for tool calls (matches original implementation)
    // Format: [{"name": "function_name", "arguments": {...}}]
    json tool_calls_schema = nullptr;
    if (has_tools) {
        auto schemas = json::array();
        foreach_function(inputs.tools, [&](const auto &, const auto & name, const json & parameters, const auto &) {
            schemas.push_back({
                {"type", "object"},
                {"properties", {
                    {"name", {
                        {"type", "string"},
                        {"const", name},
                    }},
                    {"arguments", parameters},
                }},
                {"required", json::array({"name", "arguments"})},
            });
        });
        tool_calls_schema = {
            {"type", "array"},
            {"items", schemas.size() == 1 ? schemas[0] : json{{"anyOf", schemas}}},
            {"minItems", 1},
        };
        if (!inputs.parallel_tool_calls) {
            tool_calls_schema["maxItems"] = 1;
        }
    }

    // Build the PEG parser
    bool require_tools = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED;
    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;

        // Stop tokens for Firefunction V2
        std::vector<std::string> stop_tokens = {"<|eot_id|>", "<|start_header_id|>"};

        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            if (inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED) {
                data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, " functools["});
            }

            // Build individual tool call parsers
            // Format inside array: {"name": "func_name", "arguments": {...}}
            auto tool_choice = p.choice();
            foreach_function(inputs.tools, [&](const auto &, const auto & name, const json & parameters, const auto &) {
                // Match: {"name": "tool_name", "arguments": {...}}
                // TOOL_OPEN on "{" creates a new tool call entry
                // Using << for flexible whitespace handling
                tool_choice |= p.rule("tool-" + name, p.tag(Tag::TOOL,
                    p.atomic_tag(Tag::TOOL_OPEN, p.literal("{"))
                    << "\"name\"" << ":" << "\"" + p.literal_tag(Tag::TOOL_NAME, name) + "\""
                    << "," << "\"arguments\"" << ":"
                    << p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "tool-" + name + "-args", parameters))
                    << p.atomic_tag(Tag::TOOL_CLOSE, p.literal("}"))
                ));
            });

            // Array structure: functools[ item (, item)* ]
            auto array_open = p.literal(" functools[");

            auto max_extra = inputs.parallel_tool_calls ? -1 : 0;

            // Format: [ first_item (, additional_item)* ]
            // When triggered, we always have at least one tool call
            auto items = tool_choice << p.repeat(p.literal(",") << tool_choice, 0, max_extra);
            auto tool_calls = p.trigger_rule("tool-call-root",
                array_open << items << "]"
            );

            if (require_tools) {
                return tool_calls;
            }
            return p.tag(Tag::CONTENT, p.until(" functools")) + tool_calls;
        }

        // Content only parser
        return p.tag(Tag::CONTENT, p.until_one_of(stop_tokens));
    });

    data.format = COMMON_CHAT_FORMAT_FIREFUNCTION_V2;

    // Add stop tokens
    data.additional_stops = {
        "<|eot_id|>",
        "<|start_header_id|>"
    };

    common_chat_build_peg_grammar(inputs, parser, data);

    return data;
}
