#pragma once

#include "llama.h"
#include "common.h"
#include "json-schema-to-grammar.h"

// Change JSON_ASSERT from assert() to GGML_ASSERT:
#define JSON_ASSERT GGML_ASSERT
#include "json.hpp"
#include "minja.hpp"

#include <string>
#include <vector>
#include <sstream>
#include <random>
#include <regex>

#define DEFAULT_OAICOMPAT_MODEL "gpt-3.5-turbo-0613"

using json = nlohmann::ordered_json;

// https://community.openai.com/t/openai-chat-list-of-error-codes-and-types/357791/11
enum error_type {
    ERROR_TYPE_INVALID_REQUEST,
    ERROR_TYPE_AUTHENTICATION,
    ERROR_TYPE_SERVER,
    ERROR_TYPE_NOT_FOUND,
    ERROR_TYPE_PERMISSION,
    ERROR_TYPE_UNAVAILABLE, // custom error
    ERROR_TYPE_NOT_SUPPORTED, // custom error
};

extern bool server_verbose;
extern bool server_log_json;

#ifndef SERVER_VERBOSE
#define SERVER_VERBOSE 1
#endif

#if SERVER_VERBOSE != 1
#define LOG_VERBOSE(MSG, ...)
#else
#define LOG_VERBOSE(MSG, ...)                                            \
    do                                                                   \
    {                                                                    \
        if (server_verbose)                                              \
        {                                                                \
            server_log("VERB", __func__, __LINE__, MSG, __VA_ARGS__); \
        }                                                                \
    } while (0)
#endif

#define LOG_ERROR(  MSG, ...) server_log("ERR",  __func__, __LINE__, MSG, __VA_ARGS__)
#define LOG_WARNING(MSG, ...) server_log("WARN", __func__, __LINE__, MSG, __VA_ARGS__)
#define LOG_INFO(   MSG, ...) server_log("INFO", __func__, __LINE__, MSG, __VA_ARGS__)

static inline void server_log(const char * level, const char * function, int line, const char * message, const json & extra);

template <typename T>
static T json_value(const json & body, const std::string & key, const T & default_value) {
    // Fallback null to default value
    if (body.contains(key) && !body.at(key).is_null()) {
        try {
            return body.at(key);
        } catch (NLOHMANN_JSON_NAMESPACE::detail::type_error const &) {
            std::stringstream ss;
            ss << "Wrong type supplied for parameter '" << key << "'. Expected '" << json(default_value).type_name() << "', using default value.";
            LOG_WARNING(ss.str().c_str(), body);
            return default_value;
        }
    } else {
        return default_value;
    }
}

static inline void server_log(const char * level, const char * function, int line, const char * message, const json & extra) {
    std::stringstream ss_tid;
    ss_tid << std::this_thread::get_id();
    json log = json{
        {"tid",       ss_tid.str()},
        {"timestamp", time(nullptr)},
    };

    if (server_log_json) {
        log.merge_patch({
            {"level",    level},
            {"function", function},
            {"line",     line},
            {"msg",      message},
        });

        if (!extra.empty()) {
            log.merge_patch(extra);
        }

        printf("%s\n", log.dump(-1, ' ', false, json::error_handler_t::replace).c_str());
    } else {
        char buf[1024];
        snprintf(buf, 1024, "%4s [%24s] %s", level, function, message);

        if (!extra.empty()) {
            log.merge_patch(extra);
        }
        std::stringstream ss;
        ss << buf << " |";
        for (const auto & el : log.items())
        {
            const std::string value = el.value().dump(-1, ' ', false, json::error_handler_t::replace);
            ss << " " << el.key() << "=" << value;
        }

        const std::string str = ss.str();
        printf("%.*s\n", (int)str.size(), str.data());
    }
    fflush(stdout);
}

//
// chat template utils
//

// Format given chat. If tmpl is empty, we take the template from model metadata
inline std::string format_chat(const struct llama_model * model, const std::string & tmpl, const std::vector<json> & messages, bool use_jinja) {
    std::vector<llama_chat_msg> chat;

    for (size_t i = 0; i < messages.size(); ++i) {
        const auto & curr_msg = messages[i];

        llama_chat_msg msg;
        msg.role = json_value(curr_msg, "role", std::string(""));
        msg.tool = json_value(curr_msg, "tool", std::string(""));

        if (curr_msg.contains("content")) {
            if (curr_msg["content"].is_string()) {
                msg.content = curr_msg["content"].get<std::string>();
            } else if (curr_msg["content"].is_array()) {
                for (const auto & part : curr_msg["content"]) {
                    if (part.contains("text")) {
                        msg.content += "\n" + part["text"].get<std::string>();
                    }
                }
            } else {
                throw std::runtime_error("Invalid 'content' type (ref: https://github.com/ggerganov/llama.cpp/issues/8367)");
            }
        } else {
            throw std::runtime_error("Missing 'content' (ref: https://github.com/ggerganov/llama.cpp/issues/8367)");
        }
        if (curr_msg.contains("tool_calls") && curr_msg["tool_calls"].is_array()) {
            for (const auto & tool_call : curr_msg["tool_calls"]) {
                if (json_value(tool_call, "type", std::string("")) == "function"
                        && tool_call.contains("function") && tool_call["function"].is_object()) {
                    msg.tool_calls.push_back({
                        json_value(tool_call["function"], "name", std::string("")),
                        json_value(tool_call["function"], "arguments", std::string(""))
                    });
                }
            }
        }
        chat.emplace_back(std::move(msg));
    }

    auto formatted_chat = llama_chat_apply_template(model, tmpl, chat, true, use_jinja);
    LOG_VERBOSE("formatted_chat", {{"text", formatted_chat.c_str()}});
    return formatted_chat;
}

//
// base64 utils (TODO: move to common in the future)
//

static const std::string base64_chars =
             "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
             "abcdefghijklmnopqrstuvwxyz"
             "0123456789+/";

static inline bool is_base64(uint8_t c) {
    return (isalnum(c) || (c == '+') || (c == '/'));
}

static inline std::vector<uint8_t> base64_decode(const std::string & encoded_string) {
    int i = 0;
    int j = 0;
    int in_ = 0;

    int in_len = encoded_string.size();

    uint8_t char_array_4[4];
    uint8_t char_array_3[3];

    std::vector<uint8_t> ret;

    while (in_len-- && (encoded_string[in_] != '=') && is_base64(encoded_string[in_])) {
        char_array_4[i++] = encoded_string[in_]; in_++;
        if (i == 4) {
            for (i = 0; i < 4; i++) {
                char_array_4[i] = base64_chars.find(char_array_4[i]);
            }

            char_array_3[0] = ((char_array_4[0]      ) << 2) + ((char_array_4[1] & 0x30) >> 4);
            char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
            char_array_3[2] = ((char_array_4[2] & 0x3) << 6) +   char_array_4[3];

            for (i = 0; (i < 3); i++) {
                ret.push_back(char_array_3[i]);
            }

            i = 0;
        }
    }

    if (i) {
        for (j = i; j < 4; j++) {
            char_array_4[j] = 0;
        }

        for (j = 0; j < 4; j++) {
            char_array_4[j] = base64_chars.find(char_array_4[j]);
        }

        char_array_3[0] = ((char_array_4[0]      ) << 2) + ((char_array_4[1] & 0x30) >> 4);
        char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
        char_array_3[2] = ((char_array_4[2] & 0x3) << 6) +   char_array_4[3];

        for (j = 0; j < i - 1; j++) {
            ret.push_back(char_array_3[j]);
        }
    }

    return ret;
}

//
// random string / id
//

static std::string random_string() {
    static const std::string str("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");

    std::random_device rd;
    std::mt19937 generator(rd());

    std::string result(32, ' ');

    for (int i = 0; i < 32; ++i) {
        result[i] = str[generator() % str.size()];
    }

    return result;
}

static std::string gen_chatcmplid() {
    std::stringstream chatcmplid;
    chatcmplid << "chatcmpl-" << random_string();

    return chatcmplid.str();
}

//
// other common utils
//

static size_t common_part(const std::vector<llama_token> & a, const std::vector<llama_token> & b) {
    size_t i;
    for (i = 0; i < a.size() && i < b.size() && a[i] == b[i]; i++) {}

    return i;
}

static size_t common_part(const std::string & a, const std::string & b) {
    size_t i;
    for (i = 0; i < a.size() && i < b.size() && a[i] == b[i]; i++) {}

    return i;
}

static size_t find_partial_stop_string(const std::string & stop, const std::string & text) {
    if (!text.empty() && !stop.empty()) {
        auto it = std::find(stop.rbegin(), stop.rend(), text.back());
        while (it != stop.rend()) {
            size_t length = std::distance(it, stop.rend());
            if (text.length() >= length && 0 == text.compare(text.length() - length, length, stop)) {
                return text.length() - length;
            }
            it = std::find(std::next(it), stop.rend(), text.back());
        }
    }

    return std::string::npos;
}

// TODO: reuse llama_detokenize
template <class Iter>
static std::string tokens_to_str(llama_context * ctx, Iter begin, Iter end) {
    std::string ret;
    for (; begin != end; ++begin) {
        ret += llama_token_to_piece(ctx, *begin);
    }

    return ret;
}

// format incomplete utf-8 multibyte character for output
static std::string tokens_to_output_formatted_string(const llama_context * ctx, const llama_token token) {
    std::string out = token == -1 ? "" : llama_token_to_piece(ctx, token);

    // if the size is 1 and first bit is 1, meaning it's a partial character
    //   (size > 1 meaning it's already a known token)
    if (out.size() == 1 && (out[0] & 0x80) == 0x80) {
        std::stringstream ss;
        ss << std::hex << (out[0] & 0xff);
        std::string res(ss.str());
        out = "byte: \\x" + res;
    }

    return out;
}

struct completion_token_output {
    llama_token tok;
    std::string text_to_send;

    struct token_prob {
        llama_token tok;
        float prob;
    };

    std::vector<token_prob> probs;
};

// convert a vector of completion_token_output to json
static json probs_vector_to_json(const llama_context * ctx, const std::vector<completion_token_output> & probs) {
    json out = json::array();

    for (const auto & prob : probs) {
        json probs_for_token = json::array();

        for (const auto & p : prob.probs) {
            const std::string tok_str = tokens_to_output_formatted_string(ctx, p.tok);
            probs_for_token.push_back(json {
                {"tok_str", tok_str},
                {"prob",    p.prob},
            });
        }

        const std::string tok_str = tokens_to_output_formatted_string(ctx, prob.tok);
        out.push_back(json {
            {"content", tok_str},
            {"probs",   probs_for_token},
        });
    }

    return out;
}

//
// OAI utils
//

static std::string _llama_token_to_piece(const struct llama_model * model, llama_token token, bool special) {
    std::string piece;
    piece.resize(piece.capacity());  // using string internal cache, 15 bytes + '\n'
    const int n_chars = llama_token_to_piece(model, token, &piece[0], piece.size(), 0, special);
    if (n_chars < 0) {
        piece.resize(-n_chars);
        int check = llama_token_to_piece(model, token, &piece[0], piece.size(), 0, special);
        GGML_ASSERT(check == -n_chars);
    }
    else {
        piece.resize(n_chars);
    }

    return piece;
}

std::string llama_model_meta_val_str(const struct llama_model * model, const char * key) {
    int32_t tlen = llama_model_meta_val_str(model, key, nullptr, 0);
    if (tlen > 0) {
        std::vector<char> curr_tmpl_buf(tlen + 1, 0);
        if (llama_model_meta_val_str(model, key, curr_tmpl_buf.data(), curr_tmpl_buf.size()) == tlen) {
            return std::string(curr_tmpl_buf.data(), tlen);
        }
    }
    return "";
}

static json oaicompat_completion_params_parse(
    const struct llama_model * model,
    const json & body, /* openai api json semantics */
    const std::string & chat_template_src,
    bool use_jinja) {
    json llama_params;

    llama_params["__oaicompat"] = true;

    auto eos_token = _llama_token_to_piece(model, llama_token_eos(model), /* special= */ true);
    auto bos_token = _llama_token_to_piece(model, llama_token_bos(model), /* special= */ true);

    
    // Apply chat template to the list of messages
    std::string chat_template;
    if (use_jinja) {
        chat_template = chat_template_src.empty() ? llama_model_meta_val_str(model, "tokenizer.chat_template") : chat_template_src;
        auto tools = json_value(body, "tools", json());
        if (tools.is_array() && !tools.empty() && chat_template.find("tools") == std::string::npos) {
            throw std::runtime_error("Chat template does not seem to support tools. Override the model template with --chat-template.");
        }
        auto context = minja::Context::make(json({
            {"model", json_value(body, "model", json())},
            {"messages", json_value(body, "messages", json())},
            {"tools", tools},
            {"add_generation_prompt", true},
            {"eos_token", _llama_token_to_piece(model, llama_token_eos(model), /* special= */ true)},
            {"bos_token", _llama_token_to_piece(model, llama_token_bos(model), /* special= */ true)},

            {"builtin_tools", {"wolfram_alpha", "brave_search"}},
            {"cutting_knowledge_date", "2023-04-01"},
            {"todays_date", "2024-09-03"},
        }));
        auto tmpl = minja::Parser::parse(chat_template, minja::Options {.trim_blocks = true, .lstrip_blocks = true});
        llama_params["prompt"] = tmpl->render(context);
        llama_params["chat_template"] = chat_template;
    } else {
        llama_params["prompt"] = format_chat(model, chat_template_src, body.at("messages"), use_jinja);
    }

    LOG_INFO("prompt", {{"prompt", llama_params["prompt"]}, {"grammar", llama_params["grammar"]}});

    // Handle "stop" field
    if (body.contains("stop") && body.at("stop").is_string()) {
        llama_params["stop"] = json::array({body.at("stop").get<std::string>()});
    } else {
        llama_params["stop"] = json_value(body, "stop", json::array());
    }

    // Handle "response_format" field (https://platform.openai.com/docs/api-reference/chat/create#chat-create-response_format)
    auto tool_choice = json_value(body, "tool_choice", std::string("auto"));
    std::string extra_system_message;
    if (body.contains("response_format")) {
        json response_format      = json_value(body, "response_format", json::object());
        std::string response_type = json_value(response_format, "type", std::string());
        if (response_type == "json_object") {
            // Legacy llama.cpp, llama-cpp-python and Together.ai format.
            llama_params["json_schema"] = json_value(response_format, "schema", json::object());
        } else if (response_type == "json_schema") {
            // OpenAI JSON schema format.
            auto json_schema = json_value(response_format, "json_schema", json::object());
            json schema = json_value(json_schema, "schema", json::object());
            std::string description = json_value(json_schema, "description", std::string());
            if (!description.empty()) {
                if (schema.contains("description")) {
                    throw std::runtime_error("Cannot have both a description in the json_schema object and inside its schema.");
                }
                schema["description"] = description;
            }
            bool strict = json_value(json_schema, "strict", false);
            if (strict) {
                llama_params["json_schema"] = schema;
            }
        } else if (!response_type.empty() && response_type != "text") {
            throw std::runtime_error("response_format type must be one of \"text\" or \"json_object\", but got: " + response_type);
        }
    } else if (use_jinja && tool_choice != "none" && body.contains("tools") && body["tools"].is_array()) {
        const auto & tools = body["tools"];
        bool parallel_tool_calls = json_value(body, "parallel_tool_calls", false);
        bool allow_content = tool_choice != "required";

        llama_tool_call_style style = llama_tool_call_style::MetaLlama_3_1;
        
        std::string grammar;
        std::vector<std::string> grammar_trigger_words;
        std::vector<std::string> additional_stop_words;
        std::function<bool(std::string::const_iterator &, const std::string::const_iterator &, json &)> tool_call_parser;

        tool_call_grammar(
            chat_template,
            allow_content,
            parallel_tool_calls,
            tools,
            grammar,
            grammar_trigger_words,
            additional_stop_words,
            tool_call_parser);

        for (const auto & stop : additional_stop_words) {
            llama_params["stop"].push_back(stop);
        }
        if (!grammar_trigger_words.empty()) {
            auto triggers = json::array();
            for (const auto & word : grammar_trigger_words) {
                triggers.push_back(word);
            }
            llama_params["grammar_trigger_words"] = triggers;
        }

        llama_params["grammar"] = grammar;
        llama_params["parse_tool_calls"] = true;
        llama_params["parallel_tool_calls"] = parallel_tool_calls;
    }
    
    // Params supported by OAI but unsupported by llama.cpp
    if (!use_jinja) {
        static const std::vector<std::string> unsupported_params { "tools", "tool_choice" };
        for (auto & param : unsupported_params) {
            if (body.contains(param)) {
                throw std::runtime_error("Unsupported param (--jinja mode required for tool call): " + param);
            }
        }
    }

    // Handle "n" field
    int n_choices = json_value(body, "n", 1);
    if (n_choices != 1) {
        throw std::runtime_error("Only one completion choice is allowed");
    }

    // Handle "logprobs" field
    // TODO: The response format of this option is not yet OAI-compatible, but seems like no one really using it; We may need to fix it in the future
    if (body.contains("logprobs")) {
        llama_params["n_probs"] = json_value(body, "top_logprobs", 20);
    } else if (body.contains("top_logprobs")) {
        throw std::runtime_error("top_logprobs requires logprobs to be set to true");
    }


    // Copy remaining properties to llama_params
    // This allows user to use llama.cpp-specific params like "mirostat", "tfs_z",... via OAI endpoint.
    // See "launch_slot_with_task()" for a complete list of params supported by llama.cpp
    for (const auto & item : body.items()) {
        // Exception: if "n_predict" is present, we overwrite the value specified earlier by "max_tokens"
        if (!llama_params.contains(item.key()) || item.key() == "n_predict") {
            llama_params[item.key()] = item.value();
        }
    }

    return llama_params;
}

/*
 * Parses `"<tool_call>foo</tool_call><tool_call>bar</tool_call>"` to `[foo, bar]` where foo and bar are valid JSON.
 */

struct llama_tool_call_extractor {
    std::string regex_with_group_names;
    std::vector<std::string> group_names;
    std::regex regex;

};

static bool parse_json(std::string::const_iterator & it, const std::string::const_iterator & end, json & out) {
    // // https://json.nlohmann.me/features/parsing/sax_interface/
    struct json_error_locator : public nlohmann::json_sax<json> {
        std::size_t position;
        bool found_error;

        bool parse_error(std::size_t position, const std::string & last_token, const json::exception & ex) override {
            LOG_WARNING("JSON error (Expected)", {{"position", position}, {"last_token", last_token}, {"error", ex.what()}});
            this->position = position - 1;
            this->found_error = true;
            return false;
        }
        bool null() override { return true; }
        bool boolean(bool) override { return true; }
        bool number_integer(number_integer_t) override { return true; }
        bool number_unsigned(number_unsigned_t) override { return true; }
        bool number_float(number_float_t, const string_t &) override { return true; }
        bool string(string_t &) override { return true; }
        bool binary(binary_t &) override { return true; }
        bool start_object(std::size_t) override { return true; }
        bool key(string_t &) override { return true; }
        bool end_object() override { return true; }
        bool start_array(std::size_t) override { return true; }
        bool end_array() override { return true; }
    };
    json_error_locator err_loc;
    json::sax_parse(it, end, &err_loc);

    std::string::const_iterator temptative_end;
    if (err_loc.found_error) {
        temptative_end = it + err_loc.position;
    } else {
        temptative_end = end;
    }
    std::string json_sub {it, it + err_loc.position};
    LOG_WARNING("Parsing json", {{"json_sub", json_sub}});
    try {
        out = json::parse(json_sub);
        it = temptative_end;
        return true;
    } catch (const std::exception & e) {
        LOG_WARNING("Failed to parse tool call", {{"json_sub", json_sub}, {"error", e.what()}});
        return false;
    }
}

static std::pair<std::string, json> parse_hermes_tool_calls(const std::string& input) {
    try {
        std::regex start_pattern(R"(^[\n\s]*<tool_call>)");
        std::regex middle_pattern(R"([\n\s]*</tool_call>[\n\s]*<tool_call>)");
        std::regex end_pattern(R"([\n\s]*</tool_call>[\n\s]*$)");
        
        auto end = input.end();
        std::sregex_iterator rend;
        std::sregex_iterator rit(input.begin(), end, start_pattern);
        if (rit == rend) {
            return {input, json()};
        }
        
        auto content = rit->prefix();

        json tool_calls = json::array();
        auto it = rit->suffix().first;
        while (it != end) {
            json call;
            if (!parse_json(it, end, call)) {
                LOG_WARNING("Failed to parse json tool call", {{"input", input}});
                break;
            }
            tool_calls.push_back({
                {"function", {
                    {"name", call["name"]},
                    {"arguments", call["arguments"].dump()},
                }},
            });
            rit = {it, end, middle_pattern};
            if (rit != rend) {
                it = rit->suffix().first;
            } else {
                rit = {it, end, end_pattern};
                if (rit == rend) {
                    LOG_WARNING("Malformed input, missing </tool_call>", {{"input", input}});
                }
                break;
            }
        }
        return {content, tool_calls};
    } catch (const std::exception & e) {
        LOG_WARNING("Failed to parse tool calls", {{"input", input}, {"error", e.what()}});
        return {input, json()};
    }
}

static std::pair<std::string, json> parse_llama_3_1_tool_calls(const json & request, const std::string& input) {
    static std::regex python_tag_regex(R"(^<\|python_tag\|>(.*)$)");
    std::smatch match;
    if (std::regex_search(input, match, python_tag_regex)) {
        return {match.prefix().str(), {
            {"function", {
                {"name", "ipython"},
                {"arguments", {
                    {"code", match[1].str()},
                }},
            }}
        }};
    }
    try {
        auto call = json::parse(input);
        // Only treat JSON as a tool call if it has a name attribute that matches any of the tools specified in the request.
        // There doesn't seem to be any better way to detect a tool call.
        auto name = json_value(call, "name", std::string());
        for (const auto & tool : request.at("tools")) {
            if (json_value(json_value(tool, "function", json::object()), "name", std::string()) == name) {
                return {input, call};
            }
        }
    } catch (const std::exception & e) {
        // Do nothing
    }
    return {input, json()};
}


static std::pair<std::string, json> parse_functionary_3_2_tool_calls(const std::string& input) {
    throw std::runtime_error(std::string("Tool call extraction not implemented: ") + input);
}

static std::pair<std::string, json> parse_tool_calls(const json & request, const std::string & chat_template, const std::string& input) {
    if (chat_template.find("<tool_call>") != std::string::npos) {
        return parse_hermes_tool_calls(input);
    } else if (chat_template.find("<|start_header_id|>") != std::string::npos
            && chat_template.find("<|python_tag|>") != std::string::npos) {
        return parse_llama_3_1_tool_calls(request, input);
    } else if (chat_template.find("<|start_header_id|>") != std::string::npos
            && chat_template.find(">>>all") != std::string::npos) {
        return parse_functionary_3_2_tool_calls(input);
    } else {
        throw std::runtime_error("Unsupported chat template for tool calls");
    }
}

static json format_final_response_oaicompat(const json & request, json result, const std::string & completion_id, bool streaming = false) {
    bool stopped_word        = result.count("stopped_word") != 0;
    bool stopped_eos         = json_value(result, "stopped_eos", false);
    int num_tokens_predicted = json_value(result, "tokens_predicted", 0);
    int num_prompt_tokens    = json_value(result, "tokens_evaluated", 0);
    std::string content      = json_value(result, "content", std::string(""));

    std::string finish_reason = "length";
    if (stopped_word || stopped_eos) {
        finish_reason = "stop";
    }
    auto chat_template = json_value(request, "chat_template", std::string());
    std::pair<std::string, json> content_and_tool_calls;
    json tool_calls;
    json message_content;
    if (json_value(request, "parse_tool_calls", false) && (content_and_tool_calls = parse_tool_calls(request, chat_template, content)).second.is_array()) {
        finish_reason = "tool";
        if (!content_and_tool_calls.first.empty()) {
            message_content = content_and_tool_calls.first;
        }
        tool_calls = content_and_tool_calls.second;
        LOG_INFO("Tool calls", {{"content", content}, {"message_content", message_content}, {"tool_calls", tool_calls}});
    } else {
        message_content = content;
    }

    json choices =
        streaming ? json::array({json{{"finish_reason", finish_reason},
                                        {"index", 0},
                                        {"delta", json::object()}}})
                  : json::array({json{{"finish_reason", finish_reason},
                                        {"index", 0},
                                        {"message", json{{"content", message_content},
                                                         {"tool_calls", tool_calls},
                                                         {"role", "assistant"}}}}});

    std::time_t t = std::time(0);

    json res = json {
        {"choices", choices},
        {"created", t},
        {"model",
            json_value(request, "model", std::string(DEFAULT_OAICOMPAT_MODEL))},
        {"object", streaming ? "chat.completion.chunk" : "chat.completion"},
        {"usage", json {
            {"completion_tokens", num_tokens_predicted},
            {"prompt_tokens",     num_prompt_tokens},
            {"total_tokens",      num_tokens_predicted + num_prompt_tokens}
        }},
        {"id", completion_id}
    };

    if (server_verbose) {
        res["__verbose"] = result;
    }

    if (result.contains("completion_probabilities")) {
        res["completion_probabilities"] = json_value(result, "completion_probabilities", json::array());
    }

    return res;
}

// return value is vector as there is one case where we might need to generate two responses
static std::vector<json> format_partial_response_oaicompat(json result, const std::string & completion_id) {
    if (!result.contains("model") || !result.contains("oaicompat_token_ctr")) {
        return std::vector<json>({result});
    }

    bool first = json_value(result, "oaicompat_token_ctr", 0) == 0;
    std::string modelname = json_value(result, "model", std::string(DEFAULT_OAICOMPAT_MODEL));

    bool stopped_word   = json_value(result, "stopped_word",  false);
    bool stopped_eos    = json_value(result, "stopped_eos",   false);
    bool stopped_limit  = json_value(result, "stopped_limit", false);
    std::string content = json_value(result, "content",       std::string(""));

    std::string finish_reason;
    if (stopped_word || stopped_eos) {
        finish_reason = "stop";
    }
    if (stopped_limit) {
        finish_reason = "length";
    }

    std::time_t t = std::time(0);

    json choices;

    if (!finish_reason.empty()) {
        choices = json::array({json{{"finish_reason", finish_reason},
                                    {"index", 0},
                                    {"delta", json::object()}}});
    } else {
        if (first) {
            if (content.empty()) {
                choices = json::array({json{{"finish_reason", nullptr},
                                            {"index", 0},
                                            {"delta", json{{"role", "assistant"}}}}});
            } else {
                // We have to send this as two updates to conform to openai behavior
                json initial_ret = json{{"choices", json::array({json{
                                        {"finish_reason", nullptr},
                                        {"index", 0},
                                        {"delta", json{
                                            {"role", "assistant"}
                                        }}}})},
                            {"created", t},
                            {"id", completion_id},
                            {"model", modelname},
                            {"object", "chat.completion.chunk"}};

                json second_ret = json{
                            {"choices", json::array({json{{"finish_reason", nullptr},
                                                            {"index", 0},
                                                            {"delta", json{
                                                            {"content", content}}}
                                                            }})},
                            {"created", t},
                            {"id", completion_id},
                            {"model", modelname},
                            {"object", "chat.completion.chunk"}};

                return std::vector<json>({initial_ret, second_ret});
            }
        } else {
            // Some idiosyncrasy in task processing logic makes several trailing calls
            // with empty content, we ignore these at the calee site.
            if (content.empty()) {
                return std::vector<json>({json::object()});
            }

            choices = json::array({json{
                {"finish_reason", nullptr},
                {"index", 0},
                {"delta",
                json{
                    {"content", content},
                }},
            }});
        }
    }

    json ret = json {
        {"choices", choices},
        {"created", t},
        {"id",      completion_id},
        {"model",   modelname},
        {"object",  "chat.completion.chunk"}
    };
    if (!finish_reason.empty()) {
        int num_tokens_predicted = json_value(result, "tokens_predicted", 0);
        int num_prompt_tokens    = json_value(result, "tokens_evaluated", 0);
        ret.push_back({"usage", json {
            {"completion_tokens", num_tokens_predicted},
            {"prompt_tokens",     num_prompt_tokens},
            {"total_tokens",      num_tokens_predicted + num_prompt_tokens}
        }});
    }

    return std::vector<json>({ret});
}

static json format_embeddings_response_oaicompat(const json & request, const json & embeddings) {
    json data = json::array();
    int i = 0;
    for (auto & elem : embeddings) {
        data.push_back(json{
            {"embedding", json_value(elem, "embedding", json::array())},
            {"index",     i++},
            {"object",    "embedding"}
        });
    }

    json res = json {
        {"model", json_value(request, "model", std::string(DEFAULT_OAICOMPAT_MODEL))},
        {"object", "list"},
        {"usage", json {
            {"prompt_tokens", 0},
            {"total_tokens", 0}
        }},
        {"data", data}
    };

    return res;
}

static json format_tokenizer_response(const std::vector<llama_token> & tokens) {
    return json {
        {"tokens", tokens}
    };
}

static json format_detokenized_response(const std::string & content) {
    return json {
        {"content", content}
    };
}

static json format_error_response(const std::string & message, const enum error_type type) {
    std::string type_str;
    int code = 500;
    switch (type) {
        case ERROR_TYPE_INVALID_REQUEST:
            type_str = "invalid_request_error";
            code = 400;
            break;
        case ERROR_TYPE_AUTHENTICATION:
            type_str = "authentication_error";
            code = 401;
            break;
        case ERROR_TYPE_NOT_FOUND:
            type_str = "not_found_error";
            code = 404;
            break;
        case ERROR_TYPE_SERVER:
            type_str = "server_error";
            code = 500;
            break;
        case ERROR_TYPE_PERMISSION:
            type_str = "permission_error";
            code = 403;
            break;
        case ERROR_TYPE_NOT_SUPPORTED:
            type_str = "not_supported_error";
            code = 501;
            break;
        case ERROR_TYPE_UNAVAILABLE:
            type_str = "unavailable_error";
            code = 503;
            break;
    }
    return json {
        {"code", code},
        {"message", message},
        {"type", type_str},
    };
}
