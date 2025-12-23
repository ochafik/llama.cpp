// Functionary v3.2 tool call format
// Format: >>>all\ntext>>>fn1\n{...}>>>fn2\n{...}...
// First tool call without >>>, subsequent with >>>
// Python tool can have raw code (without opening {)

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_functionary_v3_2(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;
    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_FUNCTIONARY_V3_2;

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;

    // Build PEG parser for >>>function_name\n{...} format
    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;

        // Response format parser
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            return p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema));
        }

        // Tool call parser
        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            // First tool call (without >>>)
            auto first_tool_choice = p.choice();

            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");

                if (name == "python") {
                    // Python can have raw code or JSON
                    first_tool_choice |= p.rule("first-tool-" + name, p.tag(Tag::TOOL,
                        p.tag(Tag::TOOL_OPEN, p.eps()) + p.literal_tag(Tag::TOOL_NAME, name) + "\n"
                        + (p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "tool-" + name + "-params", parameters))
                           | p.tag(Tag::TOOL_ARGS, p.until(">>>")))
                    ));
                } else {
                    // Regular JSON tool
                    first_tool_choice |= p.rule("first-tool-" + name, p.tag(Tag::TOOL,
                        p.tag(Tag::TOOL_OPEN, p.eps()) + p.literal_tag(Tag::TOOL_NAME, name) + "\n"
                        + p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "tool-" + name + "-params", parameters))
                    ));
                }
            });

            // Subsequent tool calls (with >>>)
            auto subsequent_tool_choice = p.choice();

            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");

                if (name == "python") {
                    subsequent_tool_choice |= p.rule("tool-" + name, p.tag(Tag::TOOL,
                        p.literal_tag(Tag::TOOL_OPEN, ">>>") + p.literal_tag(Tag::TOOL_NAME, name) + "\n"
                        + (p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "tool-" + name + "-params", parameters))
                           | p.tag(Tag::TOOL_ARGS, p.until(">>>")))
                    ));
                } else {
                    subsequent_tool_choice |= p.rule("tool-" + name, p.tag(Tag::TOOL,
                        p.literal_tag(Tag::TOOL_OPEN, ">>>") + p.literal_tag(Tag::TOOL_NAME, name) + "\n"
                        + p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "tool-" + name + "-params", parameters))
                    ));
                }
            });

            // Build pattern: first call or content, then subsequent tool calls
            // Format: name\n{...}  or  all\n<content>  or  all\n<content>>>>name\n{...}
            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;

            // Content marker: "all\n" followed by text until >>> or end
            auto content_marker = "all\n" + p.tag(Tag::CONTENT, p.until(">>>"));

            // First element is either content or tool call
            auto first_element = content_marker | p.repeat(first_tool_choice, min_calls, 1);

            if (inputs.parallel_tool_calls) {
                // Subsequent tool calls with >>> prefix
                auto subsequent_calls = p.repeat(subsequent_tool_choice, 0, -1);
                return p.trigger_rule("tool-call-root", first_element) << subsequent_calls << p.tag(Tag::CONTENT, p.rest());
            } else {
                // Just the first element
                return p.trigger_rule("tool-call-root", first_element) << p.tag(Tag::CONTENT, p.rest());
            }
        }

        // Content only parser
        // Handle optional "all\n" content marker used by Functionary v3.2
        auto content_with_all = "all\n" + p.tag(Tag::CONTENT, p.until_one_of({"<|eot_id|>", "<|start_header_id|>"}));
        auto content_without_all = p.tag(Tag::CONTENT, p.until_one_of({"<|eot_id|>", "<|start_header_id|>"}));
        return content_with_all | content_without_all;
    });

    data.parser = parser.save();

    if (has_tools) {

        // Build grammar
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            std::vector<std::string> first_tool_rules;
            std::vector<std::string> subsequent_tool_rules;

            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");
                builder.resolve_refs(parameters);

                std::string args_pattern = "[\\s\\S]*";
                auto args_rule = builder.add_schema(name + "-args", parameters);
                if (name == "python") {
                    args_rule = builder.add_rule(name + "-maybe-raw-args", args_rule + " | [^{] .*");
                } else {
                    args_pattern = "\\{" + args_pattern;
                }

                auto call_rule = builder.add_rule(name + "-call", "\"" + name + "\\n\" " + args_rule);
                first_tool_rules.push_back(call_rule);

                if (inputs.parallel_tool_calls) {
                    subsequent_tool_rules.push_back(builder.add_rule(name + "-call2", "\">>>\" " + call_rule));
                }

                data.grammar_triggers.push_back({
                    COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL,
                    "((?:[\\s\\S]+?>>>)?" + regex_escape(name) + "\n)" + args_pattern,
                });
            });

            data.preserved_tokens = {
                "<|end_header_id|>",
            };

            auto first_rule = first_tool_rules.empty() ? "" : builder.add_rule("first_tool_call", string_join(first_tool_rules, " | ")) + " space";
            if (inputs.parallel_tool_calls) {
                auto subsequent_rule = builder.add_rule("subsequent_tool_call", string_join(subsequent_tool_rules, " | ")) + " space";
                builder.add_rule("root", first_rule + " (" + subsequent_rule + ")*");
            } else {
                builder.add_rule("root", first_rule);
            }
        });
    }

    return data;
}
