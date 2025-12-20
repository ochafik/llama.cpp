// Functionary v3.1 (Llama 3.1 style) tool call format
// Format: <function=name>{...}</function>
// Also supports: <|python_tag|>code...

#include "chat-template-internal.h"

common_chat_params common_chat_params_init_functionary_v3_1_llama_3_1(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    std::string python_code_argument_name;
    auto has_raw_python = false;
    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();

    if (has_tools) {
        data.format = COMMON_CHAT_FORMAT_FUNCTIONARY_V3_1_LLAMA_3_1;
        data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;

        // Detect python tool with string argument
        foreach_function(inputs.tools, [&](const json & tool) {
            const auto & function = tool.at("function");
            const auto & parameters = function.at("parameters");
            std::string name = function.at("name");
            if (name == "python" || name == "ipython") {
                if (!parameters.contains("type")) {
                    throw std::runtime_error("Missing type in python tool");
                }
                has_raw_python = true;
                const auto & type = parameters.at("type");
                if (type == "object") {
                    auto properties = parameters.at("properties");
                    for (auto it = properties.begin(); it != properties.end(); ++it) {
                        if (it.value().at("type") == "string") {
                            if (!python_code_argument_name.empty()) {
                                throw std::runtime_error("Multiple string arguments found in python tool");
                            }
                            python_code_argument_name = it.key();
                        }
                    }
                    if (python_code_argument_name.empty()) {
                        throw std::runtime_error("No string argument found in python tool");
                    }
                } else if (type != "string") {
                    throw std::runtime_error("Invalid type in python tool: " + type.dump());
                }
            }
        });

        // Set up preserved tokens
        data.preserved_tokens = {};
        if (has_raw_python) {
            data.preserved_tokens.push_back("<|python_tag|>");
        }

        // Build PEG parser for <function=name>{...}</function> format
        auto parser = build_chat_peg_native_parser([&](auto & p) {
            using Tag = common_chat_peg_tag;

            // Response format parser
            if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
                return p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema));
            }

            // Tool call parser
            if (inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
                auto tool_choice = p.choice();

                foreach_function(inputs.tools, [&](const json & tool) {
                    const auto & function = tool.at("function");
                    std::string name = function.at("name");
                    auto parameters = function.at("parameters");

                    // Format: <function=name>{...}</function>
                    tool_choice |= p.rule("tool-" + name, p.tag(Tag::TOOL,
                        p.token_tag(Tag::TOOL_OPEN, "<function=")
                        + p.literal_tag(Tag::TOOL_NAME, name)
                        + ">"
                        + p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "tool-" + name + "-params", parameters))
                        + p.token_tag(Tag::TOOL_CLOSE, "</function>")
                    ));
                });

                // Add python tag support if present
                if (has_raw_python) {
                    // <|python_tag|>code... (raw python code wrapped in arguments)
                    tool_choice |= p.rule("python-raw", p.tag(Tag::TOOL,
                        p.atomic_tag(Tag::TOOL_OPEN, p.token("<|python_tag|>") + p.literal_tag(Tag::TOOL_NAME, "python"))
                        + p.tag(Tag::TOOL_ARGS, p.rest())
                    ));
                }

                auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
                auto max_calls = inputs.parallel_tool_calls ? -1 : 1;

                std::vector<std::string> delimiters = {"<function="};
                if (has_raw_python) {
                    delimiters.push_back("<|python_tag|>");
                }

                auto tool_calls = p.trigger_rule("tool-call", p.repeat(tool_choice, min_calls, max_calls));
                return p.tag(Tag::CONTENT, p.until_one_of(delimiters)) << tool_calls;
            }

            // Content only parser
            return p.tag(Tag::CONTENT, p.rest());
        });

        data.parser = parser.save();

        // Build grammar
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            std::vector<std::string> tool_rules;
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                tool_rules.push_back(builder.add_rule(name + "-call",
                    "\"<function=" + name + ">\" " +
                    builder.add_schema(name + "-args", function.at("parameters")) +
                    " \"</function>\" space"
                ));
            });
            if (has_raw_python) {
                tool_rules.push_back(builder.add_rule("python-call", "\"<|python_tag|>\" .*"));
                data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<|python_tag|>"});
            }
            auto tool_call = builder.add_rule("tool_call", string_join(tool_rules, " | ")) + " space";
            builder.add_rule("root", inputs.parallel_tool_calls ? "(" + tool_call + ")+" : tool_call);
            data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<function="});
        });
    } else {
        data.format = COMMON_CHAT_FORMAT_CONTENT_ONLY;
    }

    data.prompt = apply(tmpl, inputs);
    return data;
}
