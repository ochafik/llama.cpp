// MiniMax-M2 tool call format
// Format: <minimax:tool_call><invoke name="function"><parameter name="key">value</parameter></invoke></minimax:tool_call>
// With optional <think>...</think> reasoning blocks

#include "chat-template-internal.h"

common_chat_params common_chat_params_init_minimax_m2(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_MINIMAX_M2;

    // Handle thinking tags based on prompt ending
    if (string_ends_with(data.prompt, "<think>\n")) {
        if (!inputs.enable_thinking) {
            data.prompt += "</think>\n\n";
        } else {
            data.thinking_forced_open = true;
        }
    }

    data.preserved_tokens = {
        "<think>",
        "</think>",
        "<minimax:tool_call>",
        "</minimax:tool_call>",
        "<invoke name=",
        "</invoke>",
        "<parameter name=",
        "</parameter>",
    };

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;
    auto include_grammar = true;

    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;
        auto reasoning = p.eps();
        if (inputs.enable_thinking && extract_reasoning) {
            auto reasoning_content = p.tag(Tag::REASONING, p.until("</think>")) + ("</think>" | p.end());
            if (data.thinking_forced_open) {
                reasoning = reasoning_content;
            }
        }

        // Response format parser
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            return reasoning << p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema));
        }

        // Tool call parser
        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            auto tool_choice = p.choice();
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");

                auto schema_info = common_schema_info();
                schema_info.resolve_refs(parameters);

                // Format: <invoke name="function_name"><parameter name="key">value</parameter></invoke>
                auto tool_open = "<invoke name=\"" + p.literal_tag(Tag::TOOL_NAME, name) + "\">\n";
                auto tool_close = p.literal("</invoke>\n");
                auto args = p.sequence();
                auto arg_string = p.rule("xml-arg-string", p.until_one_of({
                    "\n</parameter>",
                    "\n<parameter name=",
                    "\n</invoke>"
                }));

                foreach_parameter(function, [&](const auto & param_name, const json & param_schema, bool is_required) {
                    auto rule_name = "tool-" + name + "-arg-" + param_name;

                    auto arg_open = "<parameter name=\"" + p.literal_tag(Tag::TOOL_ARG_NAME, param_name) + "\">\n";
                    auto arg_close = p.literal("</parameter>\n");
                    auto arg_value = p.eps();

                    if (schema_info.resolves_to_string(param_schema)) {
                        arg_value = p.tag(Tag::TOOL_ARG_STRING_VALUE, arg_string) + "\n";
                    } else {
                        arg_value = p.tag(Tag::TOOL_ARG_JSON_VALUE, p.schema(p.json(), rule_name + "-schema", param_schema));
                    }

                    auto arg_rule = p.rule(rule_name, p.atomic_tag(Tag::TOOL_ARG_OPEN, arg_open) + arg_value + p.optional(p.atomic_tag(Tag::TOOL_ARG_CLOSE, arg_close)));
                    args += p.repeat(arg_rule, /* min = */ is_required ? 1 : 0, /* max = */ 1);
                });

                tool_choice |= p.rule("tool-" + name, p.atomic_tag(Tag::TOOL_OPEN, tool_open) + args + p.atomic_tag(Tag::TOOL_CLOSE, tool_close));
            });

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_call = p.rule("tool-call", "<minimax:tool_call>\n" + tool_choice + "</minimax:tool_call>" + p.space());
            auto tool_calls = p.trigger_rule("tool-call-root", p.repeat(tool_call, /* min = */ min_calls, /* max = */ max_calls));

            return reasoning << p.tag(Tag::CONTENT, p.until("<minimax:tool_call>")) << tool_calls;
        }

        // Content only parser
        include_grammar = false;
        return reasoning << p.tag(Tag::CONTENT, p.rest());
    });

    data.parser = parser.save();

    if (include_grammar) {
        data.grammar_lazy = has_tools && inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_AUTO;

        // Build grammar using XML tool call helper
        // Note: We can't use parser.build_grammar() because the PEG parser uses optional()
        // for closing tags to handle streaming, but grammar needs required closing tags.
        static const xml_tool_call_format form {
            /* form.scope_start = */ "<minimax:tool_call>\n",
            /* form.tool_start  = */ "<invoke name=\"",
            /* form.tool_sep    = */ "\">\n",
            /* form.key_start   = */ "<parameter name=\"",
            /* form.key_val_sep = */ "\">",
            /* form.val_end     = */ "</parameter>\n",
            /* form.tool_end    = */ "</invoke>\n",
            /* form.scope_end   = */ "</minimax:tool_call>",
        };
        build_grammar_xml_tool_call(data, inputs.tools, form);
    }

    return data;
}
