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

            auto tool_choice = p.choice();
            foreach_function(inputs.tools, [&](const auto &, const auto & name, const auto & parameters, const auto & schema_info) {
                bool allow_additional = false;
                bool additional_has_schema = false;
                json additional_schema;
                if (parameters.contains("additionalProperties")) {
                    const json & additional = parameters.at("additionalProperties");
                    if (additional.is_boolean()) {
                        allow_additional = additional.get<bool>();
                    } else if (additional.is_object()) {
                        allow_additional = true;
                        additional_has_schema = true;
                        additional_schema = additional;
                    }
                }

                auto tool_open = p.space() + "<tool_call>" + p.literal_tag(Tag::TOOL_NAME, name) + "\n";
                auto tool_close = p.literal("</tool_call>");
                auto args = p.sequence();

                foreach_parameter(parameters, [&](const auto & param_name, const json & param_schema, bool is_required) {
                    auto rule_name = "tool-" + name + "-arg-" + param_name;
                    auto arg_open = "<arg_key>" + p.literal_tag(Tag::TOOL_ARG_NAME, param_name) + "</arg_key>\n<arg_value>";
                    auto arg_close = p.literal("</arg_value>") + p.optional(p.literal("\n"));
                    auto arg_value = p.schema_or_raw_string_until(rule_name + "-schema", param_schema, "</arg_value>",
                        schema_info, Tag::TOOL_ARG_STRING_VALUE, Tag::TOOL_ARG_JSON_VALUE, false);
                    auto arg_rule = p.rule(rule_name, p.atomic_tag(Tag::TOOL_ARG_OPEN, arg_open) + arg_value + p.atomic_tag(Tag::TOOL_ARG_CLOSE, arg_close));

                    int max_length = param_schema.contains("maxLength") && param_schema["maxLength"].is_number_integer()
                        ? param_schema["maxLength"].get<int>() : -1;
                    bool can_enforce = !schema_info.resolves_to_string(param_schema) || max_length > 0;
                    bool enforce_required = is_required && can_enforce;
                    args += p.repeat(arg_rule, enforce_required ? 1 : 0, 1);
                });

                if (allow_additional) {
                    auto dynamic_key = p.literal("<arg_key>") + p.tag(Tag::TOOL_ARG_NAME, p.until("</arg_key>")) + p.literal("</arg_key>\n<arg_value>");
                    auto dynamic_close = p.literal("</arg_value>") + p.optional(p.literal("\n"));
                    auto additional_value = additional_has_schema
                        ? p.schema_or_raw_string_until("glm-additional-" + name, additional_schema, "</arg_value>",
                            schema_info, Tag::TOOL_ARG_STRING_VALUE, Tag::TOOL_ARG_JSON_VALUE, false)
                        : p.tag(Tag::TOOL_ARG_STRING_VALUE, p.until("</arg_value>"));
                    auto additional_rule = p.rule("tool-" + name + "-arg-generic",
                        p.atomic_tag(Tag::TOOL_ARG_OPEN, dynamic_key) + additional_value + p.atomic_tag(Tag::TOOL_ARG_CLOSE, dynamic_close));
                    args += p.repeat(additional_rule, 0, -1);
                }

                tool_choice |= p.rule("tool-" + name, p.atomic_tag(Tag::TOOL_OPEN, tool_open) + args + p.atomic_tag(Tag::TOOL_CLOSE, tool_close) + p.space());
            });

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_calls = p.trigger_rule("tool-call-root", p.repeat(tool_choice, min_calls, max_calls));

            bool require_tools = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED;

            if (require_tools) {
                // thinking? tools
                return thinking + tool_calls;
            }

            // thinking? content? tools content?
            auto content_before = p.optional(
                p.optional(p.literal("\n"))
                + p.tag(Tag::CONTENT, p.until_one_of({"\n<tool_call>", "<tool_call>"}))
            );
            auto content_after = p.optional(p.tag(Tag::CONTENT, p.rest()));
            return thinking + content_before + tool_calls + content_after;
        }

        // No tools: thinking? content
        include_grammar = false;
        return thinking + content;
    });

    common_chat_build_peg_grammar(inputs, parser, data);

    return data;
}
