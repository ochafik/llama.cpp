// GLM 4.5 tool call format
// Format: <tool_call>function_name<arg_key>key</arg_key><arg_value>value</arg_value></tool_call>
// With optional <think>...</think> reasoning blocks

#include "chat-template-internal.h"

common_chat_params common_chat_params_init_glm_4_5(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    std::string prompt = apply(tmpl, inputs);

    // match the existing trimming behavior
    if (inputs.add_bos && string_starts_with(prompt, tmpl.bos_token())) {
        prompt.erase(0, tmpl.bos_token().size());
    }
    if (inputs.add_eos && string_ends_with(prompt, tmpl.eos_token())) {
        prompt.erase(prompt.size() - tmpl.eos_token().size());
    }
    if (string_ends_with(prompt, "<think>")) {
        if (!inputs.enable_thinking) {
            prompt += "</think>";
        } else {
            data.thinking_forced_open = true;
        }
    }

    data.prompt = prompt;
    data.format = COMMON_CHAT_FORMAT_GLM_4_5;

    // add GLM preserved tokens
    data.preserved_tokens = {
        "<|endoftext|>",
        "[MASK]",
        "[gMASK]",
        "[sMASK]",
        "<sop>",
        "<eop>",
        "<|system|>",
        "<|user|>",
        "<|assistant|>",
        "<|observation|>",
        "<|begin_of_image|>",
        "<|end_of_image|>",
        "<|begin_of_video|>",
        "<|end_of_video|>",
        "<|begin_of_audio|>",
        "<|end_of_audio|>",
        "<|begin_of_transcription|>",
        "<|end_of_transcription|>",
        "<|code_prefix|>",
        "<|code_middle|>",
        "<|code_suffix|>",
        "/nothink",
        "<think>",
        "</think>",
        "<tool_call>",
        "</tool_call>",
        "<arg_key>",
        "</arg_key>",
        "<arg_value>",
        "</arg_value>"
    };

    // extra GLM 4.5 stop word
    data.additional_stops.insert(data.additional_stops.end(), {
        "<|user|>",
        "<|observation|>"
    });

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;
    auto include_grammar = true;

    auto parser = build_chat_peg_constructed_parser([&](auto & p) {
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

                // Format: <tool_call>name<arg_key>key</arg_key><arg_value>value</arg_value></tool_call>
                auto tool_open = "\n<tool_call>" + p.literal_tag(Tag::TOOL_NAME, name) + "\n";
                auto tool_close = p.literal("</tool_call>\n");
                auto args = p.sequence();
                auto arg_string = p.rule("xml-arg-string", p.until_one_of({
                    "</arg_value>",
                    "<arg_key>",
                    "</tool_call>"
                }));

                foreach_parameter(function, [&](const auto & param_name, const json & param_schema, bool is_required) {
                    auto rule_name = "tool-" + name + "-arg-" + param_name;

                    auto arg_open = "<arg_key>" + p.literal_tag(Tag::TOOL_ARG_NAME, param_name) + "</arg_key>\n<arg_value>";
                    auto arg_close = p.literal("</arg_value>\n");
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
            auto tool_calls = p.trigger_rule("tool-call-root", p.repeat(tool_choice, /* min = */ min_calls, /* max = */ max_calls));

            return reasoning << p.tag(Tag::CONTENT, p.until("\n<tool_call>")) << tool_calls;
        }

        // Content only parser
        include_grammar = false;
        return reasoning << p.tag(Tag::CONTENT, p.rest());
    });

    data.parser = parser.save();

    if (include_grammar) {
        data.grammar_lazy = has_tools && inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_AUTO;

        // Build grammar using XML tool call helper
        static const xml_tool_call_format form {
            /* form.scope_start = */ "",
            /* form.tool_start  = */ "\n<tool_call>",
            /* form.tool_sep    = */ "\n",
            /* form.key_start   = */ "<arg_key>",
            /* form.key_val_sep = */ "</arg_key>\n<arg_value>",
            /* form.val_end     = */ "</arg_value>\n",
            /* form.tool_end    = */ "</tool_call>\n",
            /* form.scope_end   = */ "",
        };
        build_grammar_xml_tool_call(data, inputs.tools, form);
    }

    return data;
}
