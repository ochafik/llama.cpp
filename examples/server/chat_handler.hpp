#pragma once

#include "llama.h"
#include "common.h"

// Change JSON_ASSERT from assert() to GGML_ASSERT:
#define JSON_ASSERT GGML_ASSERT
#include "json.hpp"
#include "json-schema-to-grammar.h"
#include "chat_handlers.json.hpp"
#include "jinja.hpp"

#include <string>
#include <vector>
#include <sstream>
#include <random>
#include <regex>

class ChatHandler {
    using json = nlohmann::ordered_json;
    
    std::string name_;
    json model_context;
    std::unique_ptr<TemplateNode> chat_template_;
    std::unique_ptr<TemplateNode> tool_call_system_template_;
    std::unique_ptr<TemplateNode> tool_call_grammar_template_;
    std::vector<std::string> stop_words_;

public:
    class ChatSettings {
        std::string prompt;
        std::string grammar;
        std::vector<std::string> stop_words;
    };

    ChatHandler(const std::string & name, const json & model_context, const json & handler) : name_(name), model_context(model_context)
    {
        std::string chat_template = model_context["tokenizer.chat_template"];
        std::string tool_call_system_template;
        std::string tool_call_grammar_template;
        
        if (handler.contains("chat_template")) chat_template = handler["chat_template"];
        if (handler.contains("tool_call_system_template")) tool_call_system_template = handler["tool_call_system_template"];
        if (handler.contains("tool_call_grammar_template")) tool_call_grammar_template = handler["tool_call_grammar_template"];
        
        if (!chat_template.empty()) chat_template_ = JinjaParser::parse(chat_template);
        if (!tool_call_system_template.empty()) tool_call_system_template_ = JinjaParser::parse(tool_call_system_template);
        if (!tool_call_grammar_template.empty()) tool_call_grammar_template_ = JinjaParser::parse(tool_call_grammar_template);
    }

    const std::string & name() const { return name_; }

    std::pair<std::string, std::string> format_chat(const json & messages, const json & tools, const json & json_schema) const {
        std::string grammar;

        auto context_json = json({
            {"model", model_context},
            {"tools", tools},
            {"json_schema", json_schema},
        });
        // TODO: cache the grammar & the extra system message based on  the context_json so far (all but messages)
        if (tool_call_grammar_template_) {
            Value grammar_context(context_json);
            grammar = tool_call_grammar_template_->render(grammar_context);
        }
        std::string extra_system_message;
        if (tool_call_system_template_) {
            Value system_context(context_json);
            auto system_message = tool_call_system_template_->render(system_context);
            return {system_message, grammar};
        }
        if (extra_system_message.empty()) {
            context_json["messages"] = messages;
        } else {
            auto copy = messages;
            copy.insert(messages.begin(), {{"role", "system"}, {"content", extra_system_message}});
            context_json["messages"] = copy;
        }
        Value prompt_context(context_json);
        auto prompt = chat_template_->render(prompt_context);

        return {prompt, grammar};
    }

    static json build_model_context(llama_model * model) {
        auto model_context = json::object();
        std::vector<std::string> keys = {
            "general.type",
            "general.architecture",
            "general.quantization_version",
            "general.alignment",
            "general.file_type",
            "general.name",
            "general.author",
            "general.version",
            "general.organization",
            "general.finetune",
            "general.basename",
            "tokenizer.chat_template",
        };
        for (const auto & key : keys) {
            int32_t tlen = llama_model_meta_val_str(model, key.c_str(), nullptr, 0);
            if (tlen > 0) {
                std::vector<char> curr_tmpl_buf(tlen + 1, 0);
                if (llama_model_meta_val_str(model, key.c_str(), curr_tmpl_buf.data(), curr_tmpl_buf.size()) == tlen) {
                    model_context[key] = std::string(curr_tmpl_buf.data(), tlen);
                }
            }
        }
        return model_context;
    }

    static std::unique_ptr<ChatHandler> find(const std::string & name, const json & model_context) {
        if (name == "none") {
            return nullptr;
        }
        static auto handlers = json::parse(std::string((const char*) chat_handlers_json, chat_handlers_json_len));
        if (handlers.empty()) throw std::runtime_error("Empty handlers in chat_handlers.json");
        
        auto handler_names = json({"auto", "none"});
        int handler_idx = -1;
        for (size_t i = 0; i < handlers.size(); ++i) {
            const auto & handler = handlers[i];
            if (!handler.contains("name")) throw std::runtime_error("Missing 'name' in handler: " + handler.dump(2));
            std::string handler_name = handler["name"];
            handler_names.push_back(handler_name);
            if (handler_name == name) {
                handler_idx = i;
                break;
            }
        }

        if (handler_idx < 0) {
            if (std::find(handler_names.begin(), handler_names.end(), name) == handler_names.end()) {
                throw std::runtime_error("Tool call handler '" + name + "' not found. Expected one of: " + handler_names.dump());
            }
            GGML_ASSERT(name == "auto");

            for (size_t i = 0; i < handlers.size(); ++i) {
                const auto & handler = handlers[i];
                if (!handler.contains("condition")) continue;

                auto condition_eval = JinjaParser::parse(handler["condition"]);
                Value context(json({{"model", model_context}}));
                auto result = string_strip(condition_eval->render(context));
                if ((result != "True") && (result != "False")) {
                    throw std::runtime_error("Invalid tool call condition evaluation result (expected True/False): " + result);
                }
                if (result == "True") {
                    handler_idx = i;
                    break;
                }
            }
            if (handler_idx < 0) {
                throw std::runtime_error("No matching handler found for model context: " + model_context.dump(2));
            }
        }
        return std::unique_ptr<ChatHandler>(
            new ChatHandler(
                handlers[handler_idx]["name"],
                model_context, handlers[handler_idx]));
    }
};
