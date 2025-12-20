// Qwen3 Coder XML tool call format
// Format: <tool_call><function=name><parameter=key>value</parameter></function></tool_call>

#include "chat-template-internal.h"

common_chat_params common_chat_params_init_qwen3_coder_xml(const common_chat_template & tmpl, const struct templates_params & inputs) {
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

        // Response format parser
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            return p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema));
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

                // Format: <function=name><parameter=key>value</parameter></function>
                auto tool_open = "<function=" + p.literal_tag(Tag::TOOL_NAME, name) + ">\n";
                auto tool_close = p.literal("</function>\n");
                auto args = p.sequence();
                auto arg_string = p.rule("xml-arg-string", p.until_one_of({
                    "\n</parameter>",
                    "\n<parameter=",
                    "\n</function>"
                }));

                foreach_parameter(function, [&](const auto & param_name, const json & param_schema, bool is_required) {
                    auto rule_name = "tool-" + name + "-arg-" + param_name;

                    auto arg_open = "<parameter=" + p.literal_tag(Tag::TOOL_ARG_NAME, param_name) + ">\n";
                    auto arg_close = p.literal("\n</parameter>\n");
                    auto arg_value = p.eps();

                    if (schema_info.resolves_to_string(param_schema)) {
                        arg_value = p.tag(Tag::TOOL_ARG_STRING_VALUE, arg_string);
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
            auto tool_call = p.rule("tool-call", "<tool_call>\n" + tool_choice + "</tool_call>" + p.space());
            auto tool_calls = p.trigger_rule("tool-call-root", p.repeat(tool_call, /* min = */ min_calls, /* max = */ max_calls));

            return p.tag(Tag::CONTENT, p.until("<tool_call>")) << tool_calls;
        }

        // Content only parser
        include_grammar = false;
        return p.tag(Tag::CONTENT, p.rest());
    });

    data.parser = parser.save();

    if (include_grammar) {
        data.grammar_lazy = has_tools && inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_AUTO;

        // Build grammar using XML tool call helper
        static const xml_tool_call_format form {
            /* form.scope_start = */ "<tool_call>\n",
            /* form.tool_start  = */ "<function=",
            /* form.tool_sep    = */ ">\n",
            /* form.key_start   = */ "<parameter=",
            /* form.key_val_sep = */ ">\n",
            /* form.val_end     = */ "\n</parameter>\n",
            /* form.tool_end    = */ "</function>\n",
            /* form.scope_end   = */ "</tool_call>",
        };
        build_grammar_xml_tool_call(data, inputs.tools, form);
    }

    return data;
}
