// Functionary v3.2 tool call format
// Format: >>>all\ntext>>>fn1\n{...}>>>fn2\n{...}...
// ALL tool calls use >>> prefix (template generates >>> for every call)
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

        // Tool call parser: ALL tool calls use >>> prefix
        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            // Tool call choice with >>> prefix for all calls
            auto tool_choice = p.choice();

            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");

                if (name == "python") {
                    // Python can have raw code or JSON
                    tool_choice |= p.rule("tool-" + name, p.tag(Tag::TOOL,
                        p.literal_tag(Tag::TOOL_OPEN, ">>>") + p.literal_tag(Tag::TOOL_NAME, name) + "\n"
                        + (p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "tool-" + name + "-params", parameters))
                           | p.tag(Tag::TOOL_ARGS, p.until(">>>")))
                    ));
                } else {
                    // Regular JSON tool
                    tool_choice |= p.rule("tool-" + name, p.tag(Tag::TOOL,
                        p.literal_tag(Tag::TOOL_OPEN, ">>>") + p.literal_tag(Tag::TOOL_NAME, name) + "\n"
                        + p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "tool-" + name + "-params", parameters))
                    ));
                }
            });

            // Build pattern: content with "all\n" marker, then tool calls with >>> prefix
            // Format: all\n<content>>>>name\n{...}>>>name2\n{...}
            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;

            // Content marker: "all\n" followed by text until >>>
            auto content_marker = "all\n" + p.tag(Tag::CONTENT, p.until(">>>"));

            // Tool calls with >>> prefix
            auto tool_calls = p.repeat(tool_choice, min_calls, max_calls);

            // Optional trailing content, stop at end tokens
            auto trailing_content = p.optional(p.tag(Tag::CONTENT, p.until_one_of({"<|eot_id|>", "<|start_header_id|>"})));

            return p.trigger_rule("tool-call-root", content_marker) << tool_calls << trailing_content;
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
            std::vector<std::string> tool_rules;

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

                // ALL tool calls use >>> prefix
                auto call_rule = builder.add_rule(name + "-call", "\">>>\" \"" + name + "\\n\" " + args_rule);
                tool_rules.push_back(call_rule);

                data.grammar_triggers.push_back({
                    COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL,
                    "(?:[\\s\\S]+?>>>)?" + regex_escape(name) + "\n" + args_pattern,
                });
            });

            data.preserved_tokens = {
                "<|end_header_id|>",
            };

            if (!tool_rules.empty()) {
                auto tool_choice = builder.add_rule("tool_call", string_join(tool_rules, " | "));
                std::string repeat = tool_choice + " ";
                if (inputs.parallel_tool_calls) {
                    repeat = "(" + tool_choice + " space)*";
                }
                builder.add_rule("root", repeat);
            }
        });
    }

    return data;
}
