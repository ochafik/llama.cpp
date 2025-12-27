// Apertus tool call format
// Format: <|tools_prefix|>[{"func_name": {"arg1": value1}}]<|tools_suffix|>
// With optional <|inner_prefix|>...<|inner_suffix|> reasoning blocks

#include "chat-parsers-internal.h"
#include <optional>

common_chat_params common_chat_params_init_apertus_peg(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    // Apertus template uses 'content.blocks' format for reasoning, not 'reasoning_content'
    // Convert reasoning_content to content.blocks format before applying template
    auto adjusted_messages = json::array();
    for (const auto & msg : inputs.messages) {
        if (msg.contains("reasoning_content") && msg.at("reasoning_content").is_string() &&
            !msg.at("reasoning_content").get<std::string>().empty()) {
            auto adjusted_message = msg;
            json blocks = json::array();
            blocks.push_back({
                {"type", "thoughts"},
                {"text", msg.at("reasoning_content")}
            });

            // Apertus template expects content to be a mapping with blocks inside
            // If there's already content, add it as a "response" block after the "thoughts" block
            if (msg.contains("content")) {
                if (msg.at("content").is_string() && !msg.at("content").get<std::string>().empty()) {
                    // Add content as a response block after thoughts
                    blocks.push_back({
                        {"type", "response"},
                        {"text", msg.at("content")}
                    });
                } else if (msg.at("content").is_object() && msg.at("content").contains("blocks")) {
                    // Merge existing blocks with our thoughts block
                    auto existing_blocks = msg.at("content").at("blocks");
                    for (const auto & block : existing_blocks) {
                        blocks.push_back(block);
                    }
                }
            }
            adjusted_message["content"] = json::object({
                {"blocks", blocks}
            });
            adjusted_message.erase("reasoning_content");
            adjusted_messages.push_back(adjusted_message);
        } else {
            adjusted_messages.push_back(msg);
        }
    }
    data.prompt = apply(tmpl, inputs, /* messages_override= */ adjusted_messages);
    data.format = COMMON_CHAT_FORMAT_APERTUS;

    // Handle thinking tags appropriately based on inputs.enable_thinking
    if (string_ends_with(data.prompt, "<|inner_prefix|>")) {
        if (!inputs.enable_thinking) {
            data.prompt += "<|inner_suffix|>";
        } else {
            data.thinking_forced_open = true;
        }
    }

    data.preserved_tokens = {
        "<|system_start|>",
        "<|system_end|>",
        "<|developer_start|>",
        "<|developer_end|>",
        "<|user_start|>",
        "<|user_end|>",
        "<|assistant_start|>",
        "<|assistant_end|>",
        "<|inner_prefix|>",
        "<|inner_suffix|>",
        "<|tools_prefix|>",
        "<|tools_suffix|>",
    };

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;
    
    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;
        auto reasoning = p.eps();
        if (inputs.enable_thinking && extract_reasoning) {
            auto reasoning_content = p.tag(Tag::REASONING, p.until("<|inner_suffix|>")) + ("<|inner_suffix|>" | p.end());
            if (data.thinking_forced_open) {
                reasoning = reasoning_content;
            } else {
                reasoning = p.optional("<|inner_prefix|>" + reasoning_content);
            }
        }

        // Response format parser
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            return reasoning << p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema));
        }

        // Tool call parser - short form JSON array format
        // Format: <|tools_prefix|>[{"func_name": {...}}]<|tools_suffix|>
        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            // Set triggers only in AUTO mode (not REQUIRED)
            if (inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED) {
                data.grammar_triggers = {{COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL,
                    // If thinking_forced_open, then we capture the <|inner_suffix|> tag in the grammar
                    std::string(data.thinking_forced_open ?
                        "[\\s\\S]*?(<\\|inner_suffix\\|>\\s*)" :
                        "(?:<\\|inner_prefix\\|>[\\s\\S]*?<\\|inner_suffix\\|>\\s*)?") +
                    "(<\\|tools_prefix\\|>)[\\s\\S]*"}};
            }

            auto tool_calls = build_json_tool_calls_peg_parser(
                p,
                inputs,
                p.literal("<|tools_prefix|>"),
                std::nullopt,
                p.literal("<|tools_suffix|>"));

            if (inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED) {
                return p.optional(reasoning) << tool_calls;
            }
            return reasoning << p.tag(Tag::CONTENT, p.until("<|tools_prefix|>")) << tool_calls;
        }

        return reasoning << p.tag(Tag::CONTENT, p.rest());
    });

    common_chat_build_peg_grammar(inputs, parser, data);

    return data;
}
