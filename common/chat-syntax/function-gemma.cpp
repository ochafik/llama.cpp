// FunctionGemma tool call format
// Format: <start_function_call>call:name{key:<escape>value<escape>,key2:123}<end_function_call>
// String values are wrapped with <escape> tokens, non-string values are raw.

#include "chat-template-internal.h"

common_chat_params common_chat_params_init_function_gemma(const common_chat_template & tmpl, const struct templates_params & params) {
    common_chat_params data;
    data.grammar_lazy = params.tools.is_array() && !params.tools.empty() && params.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;

    data.prompt = apply(tmpl, params);
    data.format = COMMON_CHAT_FORMAT_FUNCTION_GEMMA;

    data.preserved_tokens = {
        "<start_function_call>",
        "<end_function_call>",
        "<start_function_response>",
        "<end_function_response>",
        "<escape>",
    };

    data.additional_stops.push_back("<end_function_call>");

    bool has_tools = params.tools.is_array() && !params.tools.empty();

    // Build the PEG parser for FunctionGemma format
    // Format: <start_function_call>call:name{key:<escape>value<escape>,key2:123}<end_function_call>
    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;

        // Token-aware parsers for FunctionGemma special tokens
        auto escape = p.token("<escape>");
        auto start_function_call = p.token("<start_function_call>");
        auto end_function_call = p.token("<end_function_call>");

        // Identifier pattern: [a-zA-Z_][a-zA-Z0-9_]*
        auto identifier = p.chars("a-zA-Z_", 1, 1) + p.chars("a-zA-Z0-9_", 0, -1);

        // Argument name: alphanumeric identifier before ':'
        auto arg_name = p.atomic_tag(Tag::TOOL_ARG_NAME, identifier);

        // String value: <escape>...<escape> with content captured
        // Token-aware matching ensures we don't match partial token sequences
        auto string_value = escape + p.tag(Tag::TOOL_ARG_STRING_VALUE, p.until_token("<escape>")) + escape;

        // JSON value: raw number, boolean, null, array, or object (without escape delimiters)
        auto json_value = p.tag(Tag::TOOL_ARG_JSON_VALUE, p.json());

        // An argument is: name:(string_value | json_value)
        auto arg = p.tag(Tag::TOOL_ARG, arg_name + ":" + (string_value | json_value));

        // Arguments list: {arg1,arg2,...} or {}
        auto args = "{" + p.optional(arg + p.zero_or_more("," + arg)) + "}";

        // Tool name: alphanumeric identifier after "call:"
        auto tool_name = p.atomic_tag(Tag::TOOL_NAME, identifier);

        // Tool call: <start_function_call>call:name{...}<end_function_call>
        auto tool_call = p.tag(Tag::TOOL,
            p.atomic_tag(Tag::TOOL_OPEN, start_function_call + "call:")
            + tool_name
            + args
            + p.atomic_tag(Tag::TOOL_CLOSE, end_function_call)
        );

        // Content before tool calls (token-aware matching)
        auto content = p.tag(Tag::CONTENT, p.until_token("<start_function_call>"));

        if (has_tools && params.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            int min_calls = params.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            int max_calls = params.parallel_tool_calls ? -1 : 1;
            return content + p.repeat(tool_call, min_calls, max_calls);
        }

        // Content only
        return p.tag(Tag::CONTENT, p.rest());
    });

    data.parser = parser.save();

    if (has_tools) {
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            std::vector<std::string> tool_rules;

            foreach_function(params.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                const auto & parameters = function.at("parameters");

                // Build parameter rules for this function
                std::vector<std::string> param_rules;
                if (parameters.contains("properties")) {
                    const auto & props = parameters.at("properties");
                    std::set<std::string> required_set;
                    if (parameters.contains("required")) {
                        for (const auto & r : parameters.at("required")) {
                            required_set.insert(r.get<std::string>());
                        }
                    }

                    for (auto it = props.begin(); it != props.end(); ++it) {
                        std::string param_name = it.key();
                        const auto & prop = it.value();

                        // Determine if this is a string type
                        bool is_string = prop.contains("type") && prop.at("type") == "string";
                        bool is_required = required_set.count(param_name) > 0;

                        std::string value_rule;
                        if (is_string) {
                            // String values use <escape>...</escape> delimiters
                            // Content inside can be any chars except <escape>
                            value_rule = "\"<escape>\" [^<]* \"<escape>\"";
                        } else {
                            // Non-string values are raw (numbers, booleans, etc.)
                            // Use JSON value rule for flexibility
                            value_rule = builder.add_schema(name + "_" + param_name + "_value", prop);
                        }

                        std::string param_rule = "\"" + param_name + ":\" " + value_rule;
                        if (!is_required) {
                            param_rule = "( " + param_rule + " )?";
                        }
                        param_rules.push_back(param_rule);
                    }
                }

                // Build function rule: call:name{param1:val1,param2:val2}
                std::string params_content;
                if (param_rules.empty()) {
                    params_content = "";
                } else {
                    // Join parameters with comma
                    params_content = param_rules[0];
                    for (size_t i = 1; i < param_rules.size(); ++i) {
                        params_content += " \",\" " + param_rules[i];
                    }
                }

                std::string fn_rule = "\"call:" + name + "{\" " + params_content + " \"}\"";
                std::string rule_name = builder.add_rule(name + "_call", fn_rule);
                tool_rules.push_back(rule_name);
            });

            // Root rule: <start_function_call>...tool_call...<end_function_call>
            std::string tool_call_alt = tool_rules.size() == 1 ? tool_rules[0] : "( " + string_join(tool_rules, " | ") + " )";
            std::string root_rule = "\"<start_function_call>\" " + tool_call_alt + " \"<end_function_call>\"";

            if (params.parallel_tool_calls) {
                // Allow multiple tool calls
                builder.add_rule("root", "( " + root_rule + " )+");
            } else {
                builder.add_rule("root", root_rule);
            }
        });

        data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<start_function_call>"});
    }

    return data;
}
