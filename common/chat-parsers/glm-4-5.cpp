// GLM 4.5 tool call format
// Format: <tool_call>function_name<arg_key>key</arg_key><arg_value>value</arg_value></tool_call>
// With optional <think>...</think> reasoning blocks

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_glm_4_5_peg(const common_chat_template & tmpl, const struct templates_params & inputs) {
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

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty() && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE;
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;
    auto include_grammar = true;

    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;

        // =============================================================
        // root ::= thinking? (tools | content)
        // content ::= json_schema | rest
        // =============================================================

        // THINKING - optional reasoning block at the start
        auto thinking = [&]() {
            if (!extract_reasoning) {
                return p.eps();
            }
            if (data.thinking_forced_open) {
                // Prompt ends with <think>, expect content until </think>
                return p.optional(p.literal("\n"))
                     + p.tag(Tag::REASONING, p.until("</think>"))
                     + ("</think>" | p.end());
            }
            // Optional <think>...</think> block
            return p.optional(
                p.optional(p.literal("\n"))
                + "<think>" + p.tag(Tag::REASONING, p.until("</think>")) + "</think>"
            );
        }();

        // CONTENT - either json_schema or rest (both allow optional leading newline)
        auto content = [&]() {
            if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
                return p.optional(p.literal("\n")) + p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema));
            }
            return p.optional(p.literal("\n")) + p.tag(Tag::CONTENT, p.rest());
        }();

        // TOOLS
        if (has_tools) {
            if (inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED) {
                data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<tool_call>"});
            }

            auto tool_call_start = p.space() + "<tool_call>";
            auto tool_call_name_params_sep = p.space();
            auto tool_call_end = p.space() + "</tool_call>";
            auto param_start = p.space() + "<arg_key>";
            auto param_name_value_sep = "</arg_key>" + p.space() + "<arg_value>";
            auto param_end = "</arg_value>\n";

            auto tool_call = p.choice();
            foreach_function(inputs.tools, [&](const auto &, const auto & name, const json & parameters, const auto & schema_info) {
                auto args = p.sequence();
                foreach_parameter(p, parameters, [&](const std::string & param_name, const common_peg_parser & param_p, const json & param_schema, ParameterType param_type) {
                    auto arg = p.rule("tool-" + name + "-arg-" + param_name,
                        p.tag(Tag::TOOL_ARG_OPEN, param_start)
                        + p.tag(Tag::TOOL_ARG_NAME, param_p)
                        + param_name_value_sep
                        + p.schema_or_raw_string_until("tool-" + name + "-arg-" + param_name + "-schema", param_schema, param_end,
                            schema_info, Tag::TOOL_ARG_STRING_VALUE, Tag::TOOL_ARG_JSON_VALUE, true)
                        + p.literal_tag(Tag::TOOL_ARG_CLOSE, param_end));
                    switch (param_type) {
                        case ParameterType::Required:
                            args += arg;
                            break;
                        case ParameterType::Optional:
                            args += p.optional(arg);
                            break;
                        case ParameterType::Additional:
                            args += p.repeat(arg, 0, -1);
                            break;
                        default:
                            throw std::runtime_error("Unhandled param type");
                    }
                });

                tool_call |= p.rule("tool-" + name,
                    p.tag(Tag::TOOL_OPEN, tool_call_start)
                    + p.literal_tag(Tag::TOOL_NAME, name)
                    + tool_call_name_params_sep
                    + args
                    + p.tag(Tag::TOOL_CLOSE, tool_call_end));
            });

            auto tool_calls = tool_call + p.repeat(tool_call, 0, inputs.parallel_tool_calls ? -1 : 0);
            
            if (inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED) {
                // thinking? space? tools
                return thinking + p.space() + tool_calls;
            }

            // thinking? content? space? tools content?
            auto content_before = p.optional(
                p.optional(p.literal("\n"))
                + p.tag(Tag::CONTENT, p.until_one_of({"\n<tool_call>", "<tool_call>"}))
            );
            auto content_after = p.optional(p.tag(Tag::CONTENT, p.rest()));
            return thinking + content_before + p.space() + tool_calls + content_after;
        }

        // No tools: thinking? content
        include_grammar = false;
        return thinking + content;
    });

    common_chat_build_peg_grammar(inputs, parser, data);
    data.format = COMMON_CHAT_FORMAT_PEG_CONSTRUCTED;

    return data;
}
