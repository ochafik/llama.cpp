// Seed OSS tool call format
// Format: <seed:tool_call><function=name><parameter=key>value</parameter></function></seed:tool_call>
// With optional <seed:think>...</seed:think> reasoning blocks

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_seed_oss(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_SEED_OSS;

    // Handle thinking tags appropriately based on inputs.enable_thinking
    if (string_ends_with(data.prompt, "<seed:think>")) {
        if (!inputs.enable_thinking) {
            data.prompt += "</seed:think>";
        } else {
            data.thinking_forced_open = true;
        }
    }

    data.preserved_tokens = {
        "<seed:think>",
        "</seed:think>",
        "<seed:tool_call>",
        "</seed:tool_call>",
        "<function=",
        "</function>",
        "<parameter=",
        "</parameter>",
        "<seed:eos>",
    };

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;
    auto include_grammar = true;

    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;
        auto reasoning = p.eps();
        auto eos = p.optional(p.literal("<seed:eos>"));
        if (inputs.enable_thinking && extract_reasoning) {
            auto reasoning_content = p.tag(Tag::REASONING, p.until("</seed:think>")) + ("</seed:think>" | p.end());
            if (data.thinking_forced_open) {
                reasoning = reasoning_content;
            } else {
                // Handle optional <seed:think>...</seed:think> at start of output
                reasoning = p.optional("<seed:think>" + reasoning_content);
            }
        }

        // Response format parser
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            return reasoning << p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema));
        }

        // Tool call parser
        bool require_tools = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED;
        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            auto tool_choice = p.choice();
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");

                auto schema_info = common_schema_info();
                schema_info.resolve_refs(parameters);

                bool allow_additional = false;
                bool additional_has_schema = false;
                json additional_schema;
                if (parameters.contains("additionalProperties")) {
                    const auto & additional = parameters.at("additionalProperties");
                    if (additional.is_boolean()) {
                        allow_additional = additional.get<bool>();
                    } else if (additional.is_object()) {
                        allow_additional = true;
                        additional_has_schema = true;
                        additional_schema = additional;
                    }
                }

                auto tool_open = "<function=" + p.literal_tag(Tag::TOOL_NAME, name) + ">";
                auto tool_close = p.literal("</function>");
                auto args = p.sequence();

                foreach_parameter(function, [&](const auto & param_name, const json & param_schema, bool is_required) {
                    auto rule_name = "tool-" + name + "-arg-" + param_name;

                    auto arg_open = "<parameter=" + p.literal_tag(Tag::TOOL_ARG_NAME, param_name) + ">";
                    auto arg_close = p.literal("</parameter>");
                    auto arg_value = p.eps();

                    if (schema_info.resolves_to_string(param_schema)) {
                        arg_value = p.tag(Tag::TOOL_ARG_STRING_VALUE, p.until("</parameter>"));
                    } else {
                        arg_value = p.tag(Tag::TOOL_ARG_JSON_VALUE, p.schema(p.json(), rule_name + "-schema", param_schema));
                    }

                    auto arg_rule = p.rule(rule_name,
                        p.atomic_tag(Tag::TOOL_ARG_OPEN, arg_open)
                        + arg_value
                        + p.atomic_tag(Tag::TOOL_ARG_CLOSE, arg_close)
                        + p.space());
                    args += p.repeat(arg_rule, /* min = */ 0, /* max = */ 1);
                });

                if (allow_additional) {
                    auto dynamic_name = p.tag(Tag::TOOL_ARG_NAME, p.until(">"));
                    auto additional_value = p.choice();
                    if (additional_has_schema) {
                        if (schema_info.resolves_to_string(additional_schema)) {
                            additional_value |= p.tag(Tag::TOOL_ARG_STRING_VALUE, p.until("</parameter>"));
                        } else {
                            additional_value |= p.tag(Tag::TOOL_ARG_JSON_VALUE,
                                p.schema(p.json(), "seed-oss-additional-" + name, additional_schema));
                        }
                    } else {
                        additional_value |= p.tag(Tag::TOOL_ARG_STRING_VALUE, p.until("</parameter>"));
                    }

                    auto additional_rule = p.rule("seed-parameter-generic-" + name,
                        p.atomic_tag(Tag::TOOL_ARG_OPEN, "<parameter=" + dynamic_name + ">")
                        + additional_value
                        + p.atomic_tag(Tag::TOOL_ARG_CLOSE, p.literal("</parameter>"))
                        + p.space());
                    args += p.repeat(additional_rule, 0, -1);
                }

                tool_choice |= p.rule("tool-" + name,
                    p.atomic_tag(Tag::TOOL_OPEN, tool_open)
                    << args
                    << p.atomic_tag(Tag::TOOL_CLOSE, tool_close));
            });

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_call = p.rule("tool-call",
                p.literal("<seed:tool_call>")
                << tool_choice
                << p.literal("</seed:tool_call>")
                + p.space());
            auto tool_calls = p.trigger_rule("tool-call-root", p.repeat(tool_call, /* min = */ min_calls, /* max = */ max_calls));

            if (require_tools) {
                return reasoning << tool_calls << eos;
            }
            auto content_before = p.tag(Tag::CONTENT, p.until("<seed:tool_call>"));
            return reasoning << content_before << tool_calls << eos;
        }

        // Content only parser
        include_grammar = false;
        return reasoning << p.tag(Tag::CONTENT, p.until("<seed:eos>")) << eos;
    });

    data.parser = parser.save();

    if (include_grammar) {
        data.grammar_lazy = has_tools && inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_AUTO;

        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                auto schema = function.at("parameters");
                builder.resolve_refs(schema);
            });
            parser.build_grammar(builder, data.grammar_lazy);
        });

        if (data.grammar_lazy) {
            data.grammar_triggers = {
                {COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<seed:tool_call>"}
            };
        } else {
            data.grammar_triggers.clear();
        }
    }

    return data;
}
