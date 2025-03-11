#include "chat.h"
#include "common.h"
#include "json-schema-to-grammar.h"
#include "log.h"
#include "json-partial.h"
#include "minja/chat-template.hpp"
#include "minja/minja.hpp"
#include "regex-partial.h"

#include <optional>

static std::string string_diff(const std::string & last, const std::string & current) {
    if (last.empty()) {
        return current;
    }
    if (!string_starts_with(current, last)) {
        throw std::runtime_error("Invalid diff: '" + last + "' not found at start of '" + current + "'");
    }
    return current.substr(last.size());
}

std::vector<common_chat_msg_diff> common_chat_msg_diff::compute_diffs(const common_chat_msg & previous_msg, const common_chat_msg & new_msg) {
    std::vector<common_chat_msg_diff> diffs;
    // if (previous_msg.reasoning_content != current.reasoning_content) {
    //     auto & diff = diffs.emplace_back();
    //     diff.reasoning_content_delta = string_diff(previous_msg.reasoning_content, current.reasoning_content);
    // }
    if (previous_msg.content != new_msg.content) {
        auto & diff = diffs.emplace_back();
        diff.content_delta = string_diff(previous_msg.content, new_msg.content);
    }

    if (new_msg.tool_calls.size() < previous_msg.tool_calls.size()) {
        throw std::runtime_error("Invalid diff: now finding less tool calls!");
    }

    if (!previous_msg.tool_calls.empty()) {
        auto idx = previous_msg.tool_calls.size() - 1;
        const auto & pref = previous_msg.tool_calls[idx];
        const auto & newf = new_msg.tool_calls[idx];
        if (pref.name != newf.name || pref.id != newf.id) {
            throw std::runtime_error("Invalid diff: tool call mismatch!");
        }
        auto args_diff = string_diff(pref.arguments, newf.arguments);
        if (!args_diff.empty()) {
            auto & diff = diffs.emplace_back();
            diff.tool_call_index = idx;
            diff.tool_call_delta.name = newf.name;
            diff.tool_call_delta.id = newf.id;
            diff.tool_call_delta.arguments = args_diff;
        }
    }
    for (size_t idx = previous_msg.tool_calls.size(); idx < new_msg.tool_calls.size(); ++idx) {
        auto & diff = diffs.emplace_back();
        diff.tool_call_index = idx;
        diff.tool_call_delta = new_msg.tool_calls[idx];
    }
    return diffs;
}

typedef minja::chat_template common_chat_template;

struct common_chat_templates {
    bool has_explicit_template; // Model had builtin template or template overridde was specified.
    std::unique_ptr<common_chat_template> template_default; // always set (defaults to chatml)
    std::unique_ptr<common_chat_template> template_tool_use;
};

struct templates_params {
    json messages;
    json tools;
    common_chat_tool_choice tool_choice;
    json json_schema;
    bool parallel_tool_calls;
    bool stream;
    std::string grammar;
    bool add_generation_prompt = true;
    bool extract_reasoning     = true;
};

common_chat_tool_choice common_chat_tool_choice_parse_oaicompat(const std::string & tool_choice) {
    if (tool_choice == "auto") {
        return COMMON_CHAT_TOOL_CHOICE_AUTO;
    }
    if (tool_choice == "none") {
        return COMMON_CHAT_TOOL_CHOICE_NONE;
    }
    if (tool_choice == "required") {
        return COMMON_CHAT_TOOL_CHOICE_REQUIRED;
    }
    throw std::runtime_error("Invalid tool_choice: " + tool_choice);
}

template <>
std::vector<common_chat_msg> common_chat_msgs_parse_oaicompat(const json & messages) {
    std::vector<common_chat_msg> msgs;

    try {

        if (!messages.is_array()) {
            throw std::runtime_error("Expected 'messages' to be an array, got " + messages.dump());
        }

        for (const auto & message : messages) {
            if (!message.is_object()) {
                throw std::runtime_error("Expected 'message' to be an object, got " + message.dump());
            }

            common_chat_msg msg;
            if (!message.contains("role")) {
                throw std::runtime_error("Missing 'role' in message: " + message.dump());
            }
            msg.role = message.at("role");

            auto has_content = message.contains("content");
            auto has_tool_calls = message.contains("tool_calls");
            if (has_content) {
                const auto & content = message.at("content");
                if (content.is_string()) {
                    msg.content = content;
                } else if (content.is_array()) {
                    for (const auto & part : content) {
                        if (!part.contains("type")) {
                            throw std::runtime_error("Missing content part type: " + part.dump());
                        }
                        const auto & type = part.at("type");
                        if (type != "text") {
                            throw std::runtime_error("Unsupported content part type: " + type.dump());
                        }
                        common_chat_msg_content_part msg_part;
                        msg_part.type = type;
                        msg_part.text = part.at("text");
                        msg.content_parts.push_back(msg_part);
                    }
                } else if (!content.is_null()) {
                    throw std::runtime_error("Invalid 'content' type: expected string or array, got " + content.dump() + " (ref: https://github.com/ggml-org/llama.cpp/issues/8367)");
                }
            }
            if (has_tool_calls) {
                for (const auto & tool_call : message.at("tool_calls")) {
                    common_chat_tool_call tc;
                    if (!tool_call.contains("type")) {
                        throw std::runtime_error("Missing tool call type: " + tool_call.dump());
                    }
                    const auto & type = tool_call.at("type");
                    if (type != "function") {
                        throw std::runtime_error("Unsupported tool call type: " + tool_call.dump());
                    }
                    if (!tool_call.contains("function")) {
                        throw std::runtime_error("Missing tool call function: " + tool_call.dump());
                    }
                    const auto & fc = tool_call.at("function");
                    if (!fc.contains("name")) {
                        throw std::runtime_error("Missing tool call name: " + tool_call.dump());
                    }
                    tc.name = fc.at("name");
                    tc.arguments = fc.at("arguments");
                    if (tool_call.contains("id")) {
                        tc.id = tool_call.at("id");
                    }
                    msg.tool_calls.push_back(tc);
                }
            }
            if (!has_content && !has_tool_calls) {
                throw std::runtime_error("Expected 'content' or 'tool_calls' (ref: https://github.com/ggml-org/llama.cpp/issues/8367 & https://github.com/ggml-org/llama.cpp/issues/12279)");
            }
            if (message.contains("reasoning_content")) {
                msg.reasoning_content = message.at("reasoning_content");
            }
            if (message.contains("name")) {
                msg.tool_name = message.at("name");
            }
            if (message.contains("tool_call_id")) {
                msg.tool_call_id = message.at("tool_call_id");
            }

            msgs.push_back(msg);
        }
    } catch (const std::exception & e) {
        throw std::runtime_error("Failed to parse messages: " + std::string(e.what()) + "; messages = " + messages.dump(2));
    }

    return msgs;
}

template <>
json common_chat_msgs_to_json_oaicompat(const std::vector<common_chat_msg> & msgs, bool concat_typed_text) {
    json messages = json::array();
    for (const auto & msg : msgs) {
        if (!msg.content.empty() && !msg.content_parts.empty()) {
            throw std::runtime_error("Cannot specify both content and content_parts");
        }
        json jmsg {
            {"role", msg.role},
        };
        if (!msg.content.empty()) {
            jmsg["content"] = msg.content;
        } else if (!msg.content_parts.empty()) {
            if (concat_typed_text) {
                std::string text;
                for (const auto & part : msg.content_parts) {
                    if (part.type != "text") {
                        LOG_WRN("Ignoring content part type: %s\n", part.type.c_str());
                        continue;
                    }
                    if (!text.empty()) {
                        text += '\n';
                    }
                    text += part.text;
                }
                jmsg["content"] = text;
            } else {
                auto & parts = jmsg["content"] = json::array();
                for (const auto & part : msg.content_parts) {
                    parts.push_back({
                        {"type", part.type},
                        {"text", part.text},
                    });
                }
            }
        } else {
            jmsg["content"] = json(); // null
        }
        if (!msg.reasoning_content.empty()) {
            jmsg["reasoning_content"] = msg.reasoning_content;
        }
        if (!msg.tool_name.empty()) {
            jmsg["name"] = msg.tool_name;
        }
        if (!msg.tool_call_id.empty()) {
            jmsg["tool_call_id"] = msg.tool_call_id;
        }
        if (!msg.tool_calls.empty()) {
            auto & tool_calls = jmsg["tool_calls"] = json::array();
            for (const auto & tool_call : msg.tool_calls) {
                json tc {
                    {"type", "function"},
                    {"function", {
                        {"name", tool_call.name},
                        {"arguments", tool_call.arguments},
                    }},
                };
                if (!tool_call.id.empty()) {
                    tc["id"] = tool_call.id;
                }
                tool_calls.push_back(tc);
            }
        }
        messages.push_back(jmsg);
    }
    return messages;
}

template <>
std::vector<common_chat_msg> common_chat_msgs_parse_oaicompat(const std::string & messages) {
    return common_chat_msgs_parse_oaicompat(json::parse(messages));
}

template <>
std::vector<common_chat_tool> common_chat_tools_parse_oaicompat(const json & tools) {
    std::vector<common_chat_tool> result;

    try {
        if (!tools.is_null()) {
            if (!tools.is_array()) {
                throw std::runtime_error("Expected 'tools' to be an array, got " + tools.dump());
            }
            for (const auto & tool : tools) {
                if (!tool.contains("type")) {
                    throw std::runtime_error("Missing tool type: " + tool.dump());
                }
                const auto & type = tool.at("type");
                if (!type.is_string() || type != "function") {
                    throw std::runtime_error("Unsupported tool type: " + tool.dump());
                }
                if (!tool.contains("function")) {
                    throw std::runtime_error("Missing tool function: " + tool.dump());
                }

                const auto & function = tool.at("function");
                result.push_back({
                    /* .name = */ function.at("name"),
                    /* .description = */ function.at("description"),
                    /* .parameters = */ function.at("parameters").dump(),
                });
            }
        }
    } catch (const std::exception & e) {
        throw std::runtime_error("Failed to parse tools: " + std::string(e.what()) + "; tools = " + tools.dump(2));
    }

    return result;
}

template <>
std::vector<common_chat_tool> common_chat_tools_parse_oaicompat(const std::string & tools) {
    return common_chat_tools_parse_oaicompat(json::parse(tools));
}

template <>
json common_chat_tools_to_json_oaicompat(const std::vector<common_chat_tool> & tools) {
    if (tools.empty()) {
        return json();
    }

    auto result = json::array();
    for (const auto & tool : tools) {
        result.push_back({
            {"type", "function"},
            {"function", {
                {"name", tool.name},
                {"description", tool.description},
                {"parameters", json::parse(tool.parameters)},
            }},
        });
    }
    return result;
}

bool common_chat_verify_template(const std::string & tmpl, bool use_jinja) {
    if (use_jinja) {
        try {
            common_chat_msg msg;
            msg.role = "user";
            msg.content = "test";

            auto tmpls = common_chat_templates_init(/* model= */ nullptr, tmpl);

            common_chat_templates_inputs inputs;
            inputs.messages = {msg};

            common_chat_templates_apply(tmpls.get(), inputs);
            return true;
        } catch (const std::exception & e) {
            LOG_ERR("%s: failed to apply template: %s\n", __func__, e.what());
            return false;
        }
    }
    llama_chat_message chat[] = {{"user", "test"}};
    const int res = llama_chat_apply_template(tmpl.c_str(), chat, 1, true, nullptr, 0);
    return res >= 0;
}

std::string common_chat_format_single(
        const struct common_chat_templates * tmpls,
        const std::vector<common_chat_msg> & past_msg,
        const common_chat_msg & new_msg,
        bool add_ass,
        bool use_jinja) {

    common_chat_templates_inputs inputs;
    inputs.use_jinja = use_jinja;

    std::string fmt_past_msg;
    if (!past_msg.empty()) {
        inputs.messages = past_msg;
        inputs.add_generation_prompt = false;
        fmt_past_msg = common_chat_templates_apply(tmpls, inputs).prompt;
    }
    std::ostringstream ss;
    // if the past_msg ends with a newline, we must preserve it in the formatted version
    if (add_ass && !fmt_past_msg.empty() && fmt_past_msg.back() == '\n') {
        ss << "\n";
    };
    // format chat with new_msg
    inputs.messages.push_back(new_msg);
    inputs.add_generation_prompt = add_ass;
    auto fmt_new_msg = common_chat_templates_apply(tmpls, inputs).prompt;
    // get the diff part
    ss << fmt_new_msg.substr(fmt_past_msg.size(), fmt_new_msg.size() - fmt_past_msg.size());
    return ss.str();
}

std::string common_chat_format_example(const struct common_chat_templates * tmpls, bool use_jinja) {
    common_chat_templates_inputs inputs;
    inputs.use_jinja = use_jinja;
    auto add_simple_msg = [&](auto role, auto content) {
        common_chat_msg msg;
        msg.role = role;
        msg.content = content;
        inputs.messages.push_back(msg);
    };
    add_simple_msg("system",    "You are a helpful assistant");
    add_simple_msg("user",      "Hello");
    add_simple_msg("assistant", "Hi there");
    add_simple_msg("user",      "How are you?");
    return common_chat_templates_apply(tmpls, inputs).prompt;
}

#define CHATML_TEMPLATE_SRC \
    "{%- for message in messages -%}\n" \
    "  {{- '<|im_start|>' + message.role + '\n' + message.content + '<|im_end|>\n' -}}\n" \
    "{%- endfor -%}\n" \
    "{%- if add_generation_prompt -%}\n" \
    "  {{- '<|im_start|>assistant\n' -}}\n" \
    "{%- endif -%}"

void common_chat_templates_free(struct common_chat_templates * tmpls) {
    delete tmpls;
}

bool common_chat_templates_was_explicit(const struct common_chat_templates * tmpls) {
    return tmpls->has_explicit_template;
}

const char * common_chat_templates_source(const struct common_chat_templates * tmpls, const char * variant) {
    if (variant != nullptr) {
        if (strcmp(variant, "tool_use") == 0) {
            if (tmpls->template_tool_use) {
                return tmpls->template_tool_use->source().c_str();
            }
            return nullptr;
        } else {
            LOG_DBG("%s: unknown template variant: %s\n", __func__, variant);
        }
    }
    return tmpls->template_default->source().c_str();
}

common_chat_templates_ptr common_chat_templates_init(
    const struct llama_model * model,
    const std::string & chat_template_override,
    const std::string & bos_token_override,
    const std::string & eos_token_override)
{
    std::string default_template_src;
    std::string template_tool_use_src;

    bool has_explicit_template = !chat_template_override.empty();
    if (chat_template_override.empty()) {
        GGML_ASSERT(model != nullptr);
        const auto * str = llama_model_chat_template(model, /* name */ nullptr);
        if (str) {
            default_template_src = str;
            has_explicit_template = true;
        }
        str = llama_model_chat_template(model, /* name */ "tool_use");
        if (str) {
            template_tool_use_src = str;
            has_explicit_template = true;
        }
    } else {
        default_template_src = chat_template_override;
    }
    if (default_template_src.empty() || default_template_src == "chatml") {
        if (!template_tool_use_src.empty()) {
            default_template_src = template_tool_use_src;
        } else {
            default_template_src = CHATML_TEMPLATE_SRC;
        }
    }
    std::string token_bos = bos_token_override;
    std::string token_eos = eos_token_override;
    if (model) {
        const auto * vocab = llama_model_get_vocab(model);
        const auto get_token = [&](llama_token token, const char * name, const char * jinja_variable_name) {
            if (token == LLAMA_TOKEN_NULL) {
                if (default_template_src.find(jinja_variable_name) != std::string::npos
                    || template_tool_use_src.find(jinja_variable_name) != std::string::npos) {
                    LOG_WRN("common_chat_templates_init: warning: vocab does not have a %s token, jinja template won't work as intended.\n", name);
                }
                return std::string();
            }
            return common_token_to_piece(vocab, token, true);
        };
        token_bos = get_token(llama_vocab_bos(vocab), "BOS", "bos_token");
        token_eos = get_token(llama_vocab_eos(vocab), "EOS", "eos_token");
    }
    common_chat_templates_ptr tmpls(new common_chat_templates());
    tmpls->has_explicit_template = has_explicit_template;
    try {
        tmpls->template_default = std::make_unique<minja::chat_template>(default_template_src, token_bos, token_eos);
    } catch (const std::exception & e) {
        LOG_ERR("%s: failed to parse chat template (defaulting to chatml): %s \n", __func__, e.what());
        tmpls->template_default = std::make_unique<minja::chat_template>(CHATML_TEMPLATE_SRC, token_bos, token_eos);
    }
    if (!template_tool_use_src.empty()) {
        try {
            tmpls->template_tool_use = std::make_unique<minja::chat_template>(template_tool_use_src, token_bos, token_eos);
        } catch (const std::exception & e) {
            LOG_ERR("%s: failed to parse tool use chat template (ignoring it): %s\n", __func__, e.what());
        }
    }
    return tmpls;
}

std::string common_chat_format_name(common_chat_format format) {
    switch (format) {
        case COMMON_CHAT_FORMAT_CONTENT_ONLY: return "Content-only";
        case COMMON_CHAT_FORMAT_GENERIC: return "Generic";
        case COMMON_CHAT_FORMAT_MISTRAL_NEMO: return "Mistral Nemo";
        case COMMON_CHAT_FORMAT_LLAMA_3_X: return "Llama 3.x";
        case COMMON_CHAT_FORMAT_LLAMA_3_X_WITH_BUILTIN_TOOLS: return "Llama 3.x with builtin tools";
        case COMMON_CHAT_FORMAT_DEEPSEEK_R1: return "DeepSeek R1";
        case COMMON_CHAT_FORMAT_DEEPSEEK_R1_EXTRACT_REASONING: return "DeepSeek R1 (extract reasoning)";
        case COMMON_CHAT_FORMAT_FIREFUNCTION_V2: return "FireFunction v2";
        case COMMON_CHAT_FORMAT_FUNCTIONARY_V3_2: return "Functionary v3.2";
        case COMMON_CHAT_FORMAT_FUNCTIONARY_V3_1_LLAMA_3_1: return "Functionary v3.1 Llama 3.1";
        case COMMON_CHAT_FORMAT_HERMES_2_PRO: return "Hermes 2 Pro";
        case COMMON_CHAT_FORMAT_HERMES_2_PRO_EXTRACT_REASONING: return "Hermes 2 Pro (extract reasoning)";
        case COMMON_CHAT_FORMAT_COMMAND_R7B: return "Command R7B";
        case COMMON_CHAT_FORMAT_COMMAND_R7B_EXTRACT_REASONING: return "Command R7B (extract reasoning)";
        default:
            throw std::runtime_error("Unknown chat format");
    }
}

static bool parse_json(std::string::const_iterator & it, const std::string::const_iterator & end, json & out) {
    // // https://json.nlohmann.me/features/parsing/sax_interface/
    struct json_error_locator : public nlohmann::json_sax<json> {
        std::size_t position;
        bool found_error;

        json_error_locator() : position(0), found_error(false) {}

        bool parse_error(std::size_t position, const std::string &, const json::exception &) override { // NOLINT
            this->position = position - 1;
            this->found_error = true;
            return false;
        }
        bool null() override { return true; } // NOLINT
        bool boolean(bool) override { return true; } // NOLINT
        bool number_integer(number_integer_t) override { return true; } // NOLINT
        bool number_unsigned(number_unsigned_t) override { return true; } // NOLINT
        bool number_float(number_float_t, const string_t &) override { return true; } // NOLINT
        bool string(string_t &) override { return true; } // NOLINT
        bool binary(binary_t &) override { return true; } // NOLINT
        bool start_object(std::size_t) override { return true; } // NOLINT
        bool key(string_t &) override { return true; } // NOLINT
        bool end_object() override { return true; }
        bool start_array(std::size_t) override { return true; } // NOLINT
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
    std::string json_sub {it, temptative_end};
    try {
        out = json::parse(json_sub);
        it = temptative_end;
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

static bool parse_literal(std::string::const_iterator & it, const std::string::const_iterator & end, const std::string & expected) {
    auto expected_it = expected.begin();
    auto tmp_it = it;
    while (tmp_it != end && expected_it != expected.end() && *tmp_it == *expected_it) {
        ++tmp_it;
        ++expected_it;
    }
    if (expected_it == expected.end()) {
        it = tmp_it;
        return true;
    }
    return false;
}

static std::optional<std::smatch> parse_pattern(std::string::const_iterator & it, const std::string::const_iterator & end, const std::regex & expected) {
    std::smatch match;
    if (std::regex_match(it, end, match, expected)) {
        it = match.suffix().first;
        return match;
    }
    return std::nullopt;
}

static void consume_spaces(std::string::const_iterator & it, const std::string::const_iterator & end) {
    while (it != end && std::isspace(*it)) {
        ++it;
    }
}

static bool process_tool_call(const std::string & name, const std::string & id, const std::string & arguments, const common_json & healed_json, common_chat_tool_call & out) {
    if (name.empty()) {
        return false;
    }

    auto marker_idx = std::string::npos;
    if (!arguments.empty() && !healed_json.healing_marker.empty()) {
        marker_idx = arguments.find(healed_json.json_healing_marker);
        if (marker_idx == std::string::npos) {
            marker_idx = arguments.find(healed_json.healing_marker);
        }
    }
    out = {
        /* .name = */ name,
        /* .arguments = */ marker_idx != std::string::npos ? arguments.substr(0, marker_idx) : arguments,
        /* .id = */ id,
    };
    if (out.arguments == "\"") {
        // This happens because of completing `:"$magic` after `"arguments"`
        out.arguments = "";
    }
    return true;
}

static bool process_tool_call(const json & tool_call, const common_json & healed_json, common_chat_tool_call & out) {
    return process_tool_call(
        tool_call.contains("name") ? tool_call.at("name") : "",
        tool_call.contains("id") ? tool_call.at("id") : "",
        tool_call.contains("arguments") ? tool_call.at("arguments").dump() : "",
        healed_json,
        out);
}

static bool parse_json_with_arguments(std::string::const_iterator & it, const std::string::const_iterator & end, bool is_partial, const std::function<bool(const std::vector<std::string> &)> & is_arguments_path, common_json & out) {
    if (!is_partial) {
        return parse_json(it, end, out.json);
    }

    std::string healing_marker = "$llama.cpp.json$";
    common_json jout;
    if (!common_json_parse(it, end, healing_marker, jout)) {
        return false;
    }
    fprintf(stderr, "Parsed json: %s\n", jout.json.dump().c_str());

    if (jout.healing_marker.empty()) {
        // No healing marker, just return the parsed json
        out.json = jout.json;
        return true;
    }
    // Healing marker found, we need to visit the json and removed objects that we didn't want to heal

    std::vector<std::string> path;
    std::function<json(const json &)> remove_unsupported_healings = [&](const json & j) {
        if (j.is_object()) {
            auto obj = json::object();
            for (const auto & [key, value] : j.items()) {
                std::string key_str = key;
                if (key_str.find(healing_marker) != std::string::npos) {
                    // Don't heal keys, and discard the rest of the object.
                    break;
                }
                path.push_back(key_str);
                auto is_args = is_arguments_path && is_arguments_path(path);
                if (is_args) {
                    obj[key] = value;
                } else if (value.is_string()) {
                    if (value.get<std::string>().find(healing_marker) == std::string::npos) {
                        obj[key] = value;
                    }
                } else {
                    obj[key] = remove_unsupported_healings(value);
                }
                path.pop_back();
            }
            return obj;
        }
        if (j.is_array()) {
            auto arr = json::array();
            for (const auto & value : j) {
                if (value.is_string()) {
                    std::string str = value;
                    if (str.find(healing_marker) != std::string::npos) {
                        // Don't heal array values, and discard the rest of the array.
                        break;
                    }
                }
                arr.push_back(remove_unsupported_healings(value));
            }
            return arr;
        }
        return j;
    };

    // if (jout.json.is_string()) {
    //     auto str = jout.json.get<std::string>();
    //     auto idx = str.find(healing_marker);
    //     if (idx != std::string::npos) {
    //         out.json = str.substr(0, idx);
    //         out.healing_marker = jout.healing_marker;
    //         out.json_healing_marker = jout.json_healing_marker;
    //         return true;
    //     }
    // }
    out.json = !is_arguments_path || is_arguments_path(path) ? jout.json : remove_unsupported_healings(jout.json);
    out.healing_marker = jout.healing_marker;
    out.json_healing_marker = jout.json_healing_marker;
    fprintf(stderr, "Half-healed json: %s\n", out.json.dump().c_str());

    return true;
}

/**
 * Takes a prefix regex that must have 1 group to capture the function name, a closing suffix, and expects json parameters in between.
 * Aggregates the prefix, suffix and in-between text into the content.
 */
static common_chat_msg parse_json_tool_calls(
    const std::string& input,
    bool is_partial,
    const std::optional<common_regex> & block_open,
    const common_regex & function_regex,
    const common_regex & close_regex,
    const std::optional<common_regex> & block_close,
    bool allow_raw_python = false) {

    common_chat_msg result;
    result.role = "assistant";

    auto it = input.begin();
    const auto end = input.end();

    if (block_open) {
        auto match = block_open->search(it, end);
        if (match.type == COMMON_REGEX_MATCH_TYPE_FULL || (is_partial && match.type == COMMON_REGEX_MATCH_TYPE_PARTIAL)) {
            result.content = std::string(it, match.groups[0].begin);
            if (is_partial && match.type == COMMON_REGEX_MATCH_TYPE_PARTIAL) {
                return result;
            }
            it = match.groups[0].end;
        } else {
            result.content = input;
            return result;
        }
    }

    while (it != end) {
        auto match = function_regex.search(it, end);
        if (match.type == COMMON_REGEX_MATCH_TYPE_NONE) {
            if (!is_partial) {
                break;
            }
        } else {
            result.content += std::string(it, match.groups[0].begin);
        }
        if (match.type == COMMON_REGEX_MATCH_TYPE_PARTIAL && is_partial) {
            return result;
        }
        
        // std::sregex_iterator rend;
        // std::sregex_iterator rit(it, end, function_regex);
        // if (rit == rend) {
        //     throw std::runtime_error("Failed to parse json tool calls");
        // }
        // auto name = rit->str(1);
        // result.content += std::string(it, rit->prefix().second);
        // it = rit->suffix().first;
        auto name = match.groups[1].str();
        it = match.groups[0].end;

        common_json json_tool_call;
        auto got_tool_call = parse_json_with_arguments(it, end, is_partial, [](const std::vector<std::string> & path) { return path.empty(); }, json_tool_call);
        if (got_tool_call) {
            match = close_regex.search(it, end);
            if (match.type == COMMON_REGEX_MATCH_TYPE_PARTIAL && is_partial) {
                return result;
            }
            if (match.type == COMMON_REGEX_MATCH_TYPE_NONE) {
                if (!is_partial) {
                    throw std::runtime_error("Malformed input, missing closing pattern: " + input);
                } else {
                    it = end;
                }
            } else {
                it = match.groups[0].end;
            }
            common_chat_tool_call tool_call;
            if (process_tool_call(name, "", json_tool_call.json.dump(), json_tool_call, tool_call)) {
                result.tool_calls.push_back(tool_call);
            }
        } else {
            if (allow_raw_python && name == "python") {
                std::string arguments;
                if (is_partial) {
                    std::string healing_magic = "$llama.cpp.json$";
                    arguments = json({{"code", std::string(it, end) + healing_magic}}).dump();
                    auto idx = arguments.find(healing_magic);
                    if (idx != std::string::npos) {
                        arguments = arguments.substr(0, idx);
                    }
                } else {
                    arguments = json({{"code", std::string(it, end)}}).dump();
                }
                common_chat_tool_call tool_call;
                if (process_tool_call(name, "", arguments, json_tool_call, tool_call)) {
                    result.tool_calls.push_back(tool_call);
                }
                // result.tool_calls.push_back({name, arguments, /* id= */ ""});
                break;
            }
            if (is_partial) {
                return result;
            }
            throw std::runtime_error("Failed to parse json tool call arguments: " + input);
        }
        consume_spaces(it, end);
        if (!got_tool_call || it != end) {
            if (is_partial) {
                return result;
            }
            throw std::runtime_error("Failed to parse json tool calls: " + std::string(it, end));
        }
    }

    if (block_close) {
        auto match = block_close->search(it, end);
        if (match.type == COMMON_REGEX_MATCH_TYPE_FULL || (is_partial && match.type == COMMON_REGEX_MATCH_TYPE_PARTIAL)) {
            result.content += std::string(it, match.groups[0].begin);
            if (is_partial && match.type == COMMON_REGEX_MATCH_TYPE_PARTIAL) {
                return result;
            }
            it = match.groups[0].end;
        } else {
            throw std::runtime_error("Failed to parse json tool calls (missing block closing)");
        }
    }

    if (it != end) {
        throw std::runtime_error("Failed to parse json tool calls (extra content found): " + std::string(it, end));
    }

    if (!result.tool_calls.empty()) {
        result.content = string_strip(result.content);
    }
    return result;
}

static common_chat_msg parse_prefixed_json_tool_call_array(const std::string& input, bool is_partial, const std::string & prefix, size_t rstrip_prefix = 0) {
    auto content_end = input.find(prefix);
    size_t tc_start = std::string::npos;

    common_chat_msg result;
    result.role = "assistant";
    if (content_end == std::string::npos) {
        throw std::runtime_error("Failed to parse json tool calls");
    } else {
        tc_start = content_end + prefix.size() - rstrip_prefix;
        result.content = input.substr(0, content_end);
        auto sub_input = input.substr(tc_start);
        std::string::const_iterator it = sub_input.begin();
        const std::string::const_iterator end = sub_input.end();
        common_json tool_calls;
        auto got_tool_calls = parse_json_with_arguments(it, end, is_partial, [](const std::vector<std::string> & path) { return path.size() == 2 && path[1] == "arguments"; }, tool_calls);
        consume_spaces(it, end);
        if (!got_tool_calls || it != end) {
            throw std::runtime_error("Failed to parse json tool calls: " + std::string(it, end));
        }
        for (const auto & tool_call : tool_calls.json) {
            common_chat_tool_call json_tool_call;
            if (!process_tool_call(tool_call, tool_calls, json_tool_call)) {
                continue;
            }
            result.tool_calls.push_back(json_tool_call);
        }
    }
    return result;
}

static void foreach_function(const json & tools, const std::function<void(const json &)> & fn) {
    for (const auto & tool : tools) {
        if (!tool.contains("type") || tool.at("type") != "function" || !tool.contains("function")) {
            LOG_INF("Skipping tool without function: %s", tool.dump(2).c_str());
            continue;
        }
        fn(tool);
    }
}

static std::string apply(
    const common_chat_template & tmpl,
    const nlohmann::ordered_json & messages,
    const nlohmann::ordered_json & tools,
    bool add_generation_prompt,
    const nlohmann::ordered_json & extra_context = nlohmann::ordered_json())
{
    minja::chat_template_inputs tmpl_inputs;
    tmpl_inputs.messages = messages;
    tmpl_inputs.tools = tools;
    tmpl_inputs.add_generation_prompt = add_generation_prompt;
    tmpl_inputs.extra_context = extra_context;
    // TODO: add flag to control date/time, if only for testing purposes.
    // tmpl_inputs.now = std::chrono::system_clock::now();

    minja::chat_template_options tmpl_opts;
    // To avoid double BOS / EOS tokens, we're manually removing begining / trailing tokens
    // instead of using `chat_template_options.use_bos_token = false`, since these tokens
    // may be needed inside the template / between messages too.
    auto result = tmpl.apply(tmpl_inputs, tmpl_opts);
    if (string_starts_with(result, tmpl.bos_token())) {
        result = result.substr(tmpl.bos_token().size());
    }
    if (string_ends_with(result, tmpl.eos_token())) {
        result = result.substr(0, result.size() - tmpl.eos_token().size());
    }
    return result;
}

static common_chat_params common_chat_params_init_generic(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    auto tool_call_schemas = json::array();
    foreach_function(inputs.tools, [&](const json & tool) {
        const auto & function = tool.at("function");
        auto tool_schema = json {
            {"type", "object"},
            {"properties", {
                {"name", {
                    {"type", "string"},
                    {"const", function.at("name")},
                }},
                {"arguments", function.at("parameters")},
            }},
            {"required", json::array({"name", "arguments"})},
        };
        if (function.contains("description")) {
            tool_schema["description"] = function.at("description");
        }
        if (inputs.parallel_tool_calls) {
            tool_schema.at("properties")["id"] = {
                {"type", "string"},
                {"minLength", 4},
            };
            tool_schema.at("required").push_back("id");
        }
        tool_call_schemas.emplace_back(tool_schema);
    });
    const auto tool_call =
        inputs.parallel_tool_calls
            ? json {
                {"type", "object"},
                {"properties", {
                    {"tool_calls", {
                        {"type", "array"},
                        {"items", tool_call_schemas.size() == 1 ? tool_call_schemas[0] : json {
                            {"anyOf", tool_call_schemas},
                        }},
                        {"minItems", 1},
                    }},
                }},
                {"required", json::array({"tool_calls"})},
            }
            : json {
                {"type", "object"},
                {"properties", {
                    {"tool_call", tool_call_schemas.size() == 1 ? tool_call_schemas[0] : json {
                        {"anyOf", tool_call_schemas},
                    }},
                }},
                {"required", json::array({"tool_call"})},
            };
    const auto schema =
        inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED
            ? json {
                {"anyOf", json::array({
                    tool_call,
                    {
                        {"type", "object"},
                        {"properties", {
                            {"response", inputs.json_schema.is_null()
                                ? json {{"type", "string"}}
                                : inputs.json_schema
                            },
                        }},
                        {"required", json::array({"response"})},
                    },
                })}
            }
            : tool_call;

    data.grammar_lazy = false;
    data.grammar = build_grammar([&](const common_grammar_builder & builder) {
        builder.add_schema("root", schema);
    });

    auto tweaked_messages = common_chat_template::add_system(
        inputs.messages,
        "Respond in JSON format, either with `tool_call` (a request to call tools) or with `response` reply to the user's request");

    data.prompt = apply(tmpl, tweaked_messages, inputs.tools.empty() ? json() : inputs.tools, inputs.add_generation_prompt);
    data.format = COMMON_CHAT_FORMAT_GENERIC;
    return data;
}
static common_chat_msg common_chat_parse_generic(const std::string & input, bool is_partial) {
    std::string::const_iterator it = input.begin();
    const std::string::const_iterator end = input.end();
    common_json data;
    auto got_data = parse_json_with_arguments(it, end, is_partial, [&](const std::vector<std::string> & path) {
        return (path.size() == 2 && path[0] == "tool_call" && path[1] == "arguments")
            || (path.size() == 3 && path[0] == "tool_calls" && path[2] == "arguments");
    }, data);
    if (!got_data || it != end) {
        throw std::runtime_error("Failed to parse json tool calls: " + input);
    }
    common_chat_msg result;
    result.role = "assistant";
    if (data.json.contains("tool_calls")) {
        for (const auto & tc : data.json.at("tool_calls")) {
            common_chat_tool_call tool_call;
            if (process_tool_call(
                tc.contains("name") ? tc.at("name") : "",
                tc.contains("id") ? tc.at("id") : "",
                tc.contains("arguments") ? tc.at("arguments").dump() : "",
                data,
                tool_call))
            {
                result.tool_calls.push_back(tool_call);
            }
        }
    } else if (data.json.contains("tool_call")) {
        const auto & tc = data.json.at("tool_call");

        common_chat_tool_call tool_call;
        if (process_tool_call(
            tc.contains("name") ? tc.at("name") : "",
            tc.contains("id") ? tc.at("id") : "",
            tc.contains("arguments") ? tc.at("arguments").dump() : "",
            data,
            tool_call))
        {
            result.tool_calls.push_back(tool_call);
        }
    } else if (data.json.contains("response")) {
        const auto & response = data.json.at("response");
        result.content = response.is_string() ? response.get<std::string>() : response.dump(2);
    } else if (!is_partial) {
        throw std::runtime_error("Failed to parse json tool calls: " + input);
    }
    return result;
}

static common_chat_params common_chat_params_init_mistral_nemo(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;
    data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;
    data.grammar = build_grammar([&](const common_grammar_builder & builder) {
        auto schemas = json::array();
        foreach_function(inputs.tools, [&](const json & tool) {
            const auto & function = tool.at("function");
            schemas.push_back({
                {"type", "object"},
                {"properties", {
                    // Important note: the model is probably trained to take a JSON stringified arguments value.
                    // It's hard to constrain that for now (while reusing the JSON schema conversion), so we're just expecting a plain object.
                    {"name", {
                        {"type", "string"},
                        {"const", function.at("name")},
                    }},
                    {"arguments", function.at("parameters")},
                    {"id", {
                        {"type", "string"},
                        // Nemo's template expects a 9-character alphanumeric ID.
                        {"pattern", "^[a-zA-Z0-9]{9}$"},
                    }},
                }},
                {"required", json::array({"name", "arguments", "id"})},
            });
        });
        auto schema = json {
            {"type", "array"},
            {"items", schemas.size() == 1 ? schemas[0] : json {{"anyOf", schemas}}},
            {"minItems", 1},
        };
        if (!inputs.parallel_tool_calls) {
            schema["maxItems"] = 1;
        }
        builder.add_rule("root", "\"[TOOL_CALLS]\" " + builder.add_schema("tool_calls", schema));
    });
    data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "[TOOL_CALLS]"});
    data.preserved_tokens = {
        "[TOOL_CALLS]",
    };
    data.prompt = apply(tmpl, inputs.messages, inputs.tools.empty() ? json() : inputs.tools, inputs.add_generation_prompt);
    data.format = COMMON_CHAT_FORMAT_MISTRAL_NEMO;
    return data;
}
static common_chat_msg common_chat_parse_mistral_nemo(const std::string & input, bool is_partial) {
    return parse_prefixed_json_tool_call_array(input, is_partial, "[TOOL_CALLS]");
}

static common_chat_params common_chat_params_init_command_r7b(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;
    data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;
    data.grammar = build_grammar([&](const common_grammar_builder & builder) {
        auto schemas = json::array();
        foreach_function(inputs.tools, [&](const json & tool) {
            const auto & function = tool.at("function");
            schemas.push_back({
                {"type", "object"},
                {"properties", {
                    {"tool_call_id", {
                        {"type", "string"},
                        // Command-R's template expects an integer string.
                        {"pattern", "^[0-9]{1,10}$"},
                    }},
                    {"tool_name", {
                        {"type", "string"},
                        {"const", function.at("name")},
                    }},
                    {"parameters", function.at("parameters")},
                }},
                {"required", json::array({"tool_call_id", "tool_name", "parameters"})},
            });
        });
        auto schema = json {
            {"type", "array"},
            {"items", schemas.size() == 1 ? schemas[0] : json {{"anyOf", schemas}}},
            {"minItems", 1},
        };
        if (!inputs.parallel_tool_calls) {
            schema["maxItems"] = 1;
        }
        builder.add_rule("root", "\"<|START_ACTION|>\" " + builder.add_schema("tool_calls", schema) + " \"<|END_ACTION|>\"");
    });
    data.grammar_triggers.push_back({
        COMMON_GRAMMAR_TRIGGER_TYPE_WORD,
        "<|START_ACTION|>",
    });
    data.preserved_tokens = {
        "<|START_ACTION|>",
        "<|END_ACTION|>",
        "<|START_RESPONSE|>",
        "<|END_RESPONSE|>",
        "<|START_THINKING|>",
        "<|END_THINKING|>",
    };
    auto adjusted_messages = json::array();
    for (const auto & msg : inputs.messages) {
        auto has_reasoning_content = msg.contains("reasoning_content") && msg.at("reasoning_content").is_string();
        auto has_tool_calls = msg.contains("tool_calls") && msg.at("tool_calls").is_array();
        if (has_reasoning_content && has_tool_calls) {
            auto adjusted_message = msg;
            adjusted_message["tool_plan"] = msg.at("reasoning_content");
            adjusted_message.erase("reasoning_content");
            adjusted_messages.push_back(adjusted_message);
        } else {
            adjusted_messages.push_back(msg);
        }
    }
    data.prompt = apply(tmpl, adjusted_messages, inputs.tools.empty() ? json() : inputs.tools, inputs.add_generation_prompt, {});
    data.format = inputs.extract_reasoning ? COMMON_CHAT_FORMAT_COMMAND_R7B_EXTRACT_REASONING : COMMON_CHAT_FORMAT_COMMAND_R7B;
    return data;
}

template <int n>
common_regex_match_type parse_any_triggered(
    std::string::const_iterator & it, const std::string::const_iterator & end,
    const std::array<
        std::pair<
            const common_regex,
            std::function<common_regex_match_type(
                std::string::const_iterator &,
                const std::string::const_iterator &,
                const common_regex_match &
            )>
        >,
        n
    > & triggered_options)
{
    auto earliest_trigger = end;
    for (const auto & option : triggered_options) {
        auto m = option.first.search(it, end);
        if (m.type == COMMON_REGEX_MATCH_TYPE_FULL) {
            return option.second(it, end, m);
        }
        if (m.type == COMMON_REGEX_MATCH_TYPE_PARTIAL) {
            if (m.groups[0].begin < earliest_trigger) {
                earliest_trigger = m.groups[0].begin;
            }
        }
    }
    if (earliest_trigger != end) {
        it = earliest_trigger;
        return COMMON_REGEX_MATCH_TYPE_PARTIAL;
    }
    return COMMON_REGEX_MATCH_TYPE_NONE;
}

static common_chat_msg common_chat_parse_command_r7b(const std::string & input, bool is_partial, bool extract_reasoning) {
    static const common_regex start_thinking_regex("<\\|START_THINKING\\|>", /* at_start= */ true);
    static const common_regex end_thinking_regex("<\\|END_THINKING\\|>");
    static const common_regex start_action_regex("<\\|START_ACTION\\|>", /* at_start= */ true);
    static const common_regex end_action_regex("<\\|END_ACTION\\|>");
    static const common_regex start_response_regex("<\\|START_RESPONSE\\|>", /* at_start= */ true);
    static const common_regex end_response_regex("<\\|END_RESPONSE\\|>");

    common_chat_msg result;
    result.role = "assistant";

    auto it = input.begin();
    const auto end = input.end();

    auto handle_return = [&](common_regex_match_type type) {
        if (type == COMMON_REGEX_MATCH_TYPE_FULL) {
            if (it != end) {
                throw std::runtime_error("Failed to parse command R7B input: " + input);
            }
            return result;
        }
        if (is_partial && type == COMMON_REGEX_MATCH_TYPE_PARTIAL) {
            return result;
        }
        throw std::runtime_error("Failed to parse command R7B input: " + input);
    };
    auto handle_response = [&]() {
        auto m = start_response_regex.search(it, end);
        if (m.type != COMMON_REGEX_MATCH_TYPE_FULL) {
            return handle_return(m.type);
        }
        it = m.groups[0].end;
        m = end_response_regex.search(it, end);
        auto content_begin = it;
        auto content_end = end;
        if ((m.type == COMMON_REGEX_MATCH_TYPE_FULL) || (is_partial && m.type == COMMON_REGEX_MATCH_TYPE_PARTIAL)) {
            content_end = m.groups[0].begin;
            it = m.groups[0].end;
        }
        result.content = std::string(content_begin, content_end);
        
        return handle_return(m.type);
    };

    common_regex_match m;
    

    m = start_thinking_regex.search(it, end);
    if (m.type != COMMON_REGEX_MATCH_TYPE_FULL) {
        // No thoughts, just the response
        return handle_response();
    }
    auto thoughts_it = it = m.groups[0].end;
    m = end_thinking_regex.search(it, end);
    if (m.type != COMMON_REGEX_MATCH_TYPE_FULL) {
        return handle_return(m.type);
    }
    result.reasoning_content = std::string(thoughts_it, m.groups[0].begin);
    it = m.groups[0].end;

    m = start_action_regex.search(it, end);
    if (m.type != COMMON_REGEX_MATCH_TYPE_FULL) {
        return handle_response();
    }
    auto actions_it = it = m.groups[0].end;
    m = end_action_regex.search(it, end);
    if (m.type != COMMON_REGEX_MATCH_TYPE_FULL) {
        return handle_return(m.type);
    }
    auto actions_str = std::string(actions_it, m.groups[0].begin);
    it = m.groups[0].end;

    if (!actions_str.empty()) {
        std::string::const_iterator it = actions_str.begin();
        const auto end = actions_str.end();
        common_json actions;
        auto got_actions = parse_json_with_arguments(it, end, is_partial, [&](const std::vector<std::string> & path) {
            return (path.size() == 2 && path[1] == "parameters");
        }, actions);
        if (!got_actions || it != end) {
            throw std::runtime_error("Failed to parse json tool calls: " + actions_str);
        }
        // auto actions = json::parse(actions_str);
        for (const auto & action : actions.json) {
            if (is_partial && (!action.contains("tool_name") || !action.contains("parameters") || !action.contains("tool_call_id"))) {
                continue;
            }
            auto arguments = action.at("parameters").dump();
            if (is_partial) {
                auto marker_idx = arguments.find(actions.json_healing_marker);
                if (marker_idx != std::string::npos) {
                    arguments.erase(marker_idx);
                }
            }
            result.tool_calls.push_back({
                /* .name = */      action.at("tool_name"),
                /* .arguments = */ arguments,
                /* .id = */        action.at("tool_call_id"),
            });
        }
    }

    return handle_return(it == end ? COMMON_REGEX_MATCH_TYPE_FULL : COMMON_REGEX_MATCH_TYPE_PARTIAL);
}

static void expect_tool_parameters(const std::string & name, const json & parameters, const std::vector<std::string> & expected_properties) {
    if (!parameters.is_object() || !parameters.contains("type") || parameters.at("type") != "object" || !parameters.contains("properties") || !parameters.contains("required")) {
        throw std::runtime_error("Parameters of tool " + name + " must be an object w/ required properties");
    }
    const auto & parameters_properties = parameters.at("properties");
    const auto & parameters_required = parameters.at("required");
    for (const auto & prop : expected_properties) {
        if (!parameters_properties.contains(prop)) {
            throw std::runtime_error("Parameters of tool " + name + " is missing property: " + prop); // NOLINT
        }
        if (std::find(parameters_required.begin(), parameters_required.end(), json(prop)) == parameters_required.end()) {
            throw std::runtime_error("Parameters of tool " + name + " must have property marked as required: " + prop); // NOLINT
        }
    }
    if (parameters_properties.size() != expected_properties.size()) {
        throw std::runtime_error("Parameters of tool " + name + " must only have these properties:" + string_join(expected_properties, ", "));
    }
}

static common_chat_params common_chat_params_init_llama_3_1_tool_calls(const common_chat_template & tmpl, const struct templates_params & inputs, bool allow_python_tag_builtin_tools) {
    auto builtin_tools = json::array();
    common_chat_params data;
    data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;
    data.grammar = build_grammar([&](const common_grammar_builder & builder) {
        std::vector<std::string> tool_rules;

        auto handle_builtin_tool = [&](const std::string & name, const json & parameters) {
            if (name == "wolfram_alpha" || name == "web_search" || name == "brave_search") {
                // https://github.com/meta-llama/llama-stack/blob/main/llama_stack/providers/remote/tool_runtime/wolfram_alpha/wolfram_alpha.py
                // https://github.com/meta-llama/llama-stack/blob/main/llama_stack/providers/remote/tool_runtime/brave_search/brave_search.py
                expect_tool_parameters(name, parameters, {"query"});
            } else if (name == "python" || name == "code_interpreter") {
                // https://github.com/meta-llama/llama-stack/blob/main/llama_stack/providers/inline/tool_runtime/code_interpreter/code_interpreter.py
                expect_tool_parameters(name, parameters, {"code"});
            } else {
                return false;
            }

            std::vector<std::string> kvs;
            for (const auto & [key, value] : parameters.at("properties").items()) {
                kvs.push_back("\"" + key + "=\" " + builder.add_schema(name + "-args-" + key, value)); // NOLINT
            }

            tool_rules.push_back(
                builder.add_rule(
                    name + "-call",
                    "\"<|python_tag|>" + name + ".call(\" " + string_join(kvs, " \", \" ") + " \")\""));
            builtin_tools.push_back(name);

            return true;
        };

        foreach_function(inputs.tools, [&](const json & tool) {
            const auto & function = tool.at("function");
            std::string name = function.at("name");
            auto parameters = function.at("parameters");
            builder.resolve_refs(parameters);

            // https://github.com/meta-llama/llama-stack/tree/main/llama_stack/providers/remote/tool_runtime
            if (allow_python_tag_builtin_tools) {
                handle_builtin_tool(name, parameters);
            }
            tool_rules.push_back(
                builder.add_rule(
                    name + "-call",
                    "\"{\" space "
                    "( \"\\\"type\\\"\"       space \":\" space \"\\\"function\\\"\"     space \",\" space )? "
                    "  \"\\\"name\\\"\"       space \":\" space \"\\\"" + name + "\\\"\" space \",\" space "
                    "  \"\\\"parameters\\\"\" space \":\" space " + builder.add_schema(name + "-args", parameters) + " "
                    "\"}\" space"));
        });
        // Small models may hallucinate function names so we match anything (*at the start*) that looks like the JSON of a function call, regardless of the name.
        data.grammar_triggers.push_back({
            COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_START,
            "\\{\\s*(?:\"type\"\\s*:\\s*\"function\"\\s*,\\s*)?\"name\"\\s*:\\s*\"", // + name + "\"[\\s\\S]*",
        });
        if (!builtin_tools.empty()) {
            data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<|python_tag|>"});
            data.preserved_tokens.push_back("<|python_tag|>");
        }
        // Allow a few empty lines on top of the usual constrained json schema space rule.
        builder.add_rule("root", string_join(tool_rules, " | "));
    });
    data.additional_stops.push_back("<|eom_id|>");
    data.prompt = apply(tmpl, inputs.messages, inputs.tools.empty() ? json() : inputs.tools, inputs.add_generation_prompt, {
        {"tools_in_user_message", false},
        {"builtin_tools", builtin_tools.empty() ? json() : builtin_tools},
    });
    data.format = allow_python_tag_builtin_tools && !builtin_tools.empty()
        ? COMMON_CHAT_FORMAT_LLAMA_3_X_WITH_BUILTIN_TOOLS
        : COMMON_CHAT_FORMAT_LLAMA_3_X;
    return data;
}
static common_chat_msg common_chat_parse_llama_3_1(const std::string & input, bool is_partial, bool with_builtin_tools = false) {
    // throw std::runtime_error("Llama 3.1 parsing not implemented yet");
    // TODO: tighten & simplify the parser, don't accept leading text context.
    static const common_regex function_regex(
        "\\s*\\{\\s*(?:\"type\"\\s*:\\s*\"function\"\\s*,\\s*)?\"name\"\\s*:\\s*\"([^\"]+)\"\\s*,\\s*\"parameters\"\\s*: ");
    static const common_regex close_regex("\\}\\s*");
    static const common_regex builtin_call_regex("<\\|python_tag\\|>(?:\\s*([^.(]+)\\s*\\.\\s*call\\s*\\(\\s*([\\w]+)\\s*=\\s*([\\s\\S]*?)\\))?");

    if (with_builtin_tools) {
        auto match = builtin_call_regex.search(input, /* as_match= */ true);
        if (match.type == COMMON_REGEX_MATCH_TYPE_FULL) {
            common_chat_msg msg;
            msg.role = "assistant";

            auto name = match.groups[1].str();
            if (name.empty()) {
                if (is_partial) {
                    return msg;
                }
                throw std::runtime_error("Failed to parse builtin tool call");
            }
            auto arg_name = match.groups[2].str();
            auto arg_value_str = match.groups[3].str();
            auto arg_value = json::parse(arg_value_str);

            msg.tool_calls.push_back({
                /* .name = */ name,
                /* .arguments = */ (json {
                    {arg_name, arg_value},
                }).dump(),
                /* .id = */ "",
            });
            return msg;
        }
    }
    return parse_json_tool_calls(input, is_partial, std::nullopt, function_regex, close_regex, std::nullopt);
}

static common_chat_params common_chat_params_init_deepseek_r1(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;
    if (inputs.tools.is_array() && !inputs.tools.empty()) {
        data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED && inputs.json_schema.is_null();
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            std::vector<std::string> tool_rules;
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");
                builder.resolve_refs(parameters);
                tool_rules.push_back(builder.add_rule(name + "-call",
                    "\"<｜tool▁call▁begin｜>function<｜tool▁sep｜>" + name + "\\n"
                    "```json\\n\" " + builder.add_schema(name + "-args", parameters) + " "
                    "\"```<｜tool▁call▁end｜>\""));
            });
            // Distill Qwen 7B & 32B models seem confused re/ syntax of their tool call opening tag,
            // so we accept common variants (then it's all constrained)
            builder.add_rule("root",
                "( \"<｜tool▁calls▁begin｜>\" | \"<｜tool_calls_begin｜>\" | \"<｜tool calls begin｜>\" | \"<｜tool\\\\_calls\\\\_begin｜>\" ) "
                "(" + string_join(tool_rules, " | ") + ")" + (inputs.parallel_tool_calls ? "*" : "") + " "
                "\"<｜tool▁calls▁end｜>\""
                " space");
            data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<｜tool▁calls▁begin｜>"});
            data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<｜tool_calls_begin｜>"});
            data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<｜tool calls begin｜>"});
            data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<｜tool\\_calls\\_begin｜>"});
            data.preserved_tokens = {
                "<think>",
                "</think>",
                "<｜tool▁calls▁begin｜>",
                "<｜tool▁call▁begin｜>",
                "<｜tool▁sep｜>",
                "<｜tool▁call▁end｜>",
                "<｜tool▁calls▁end｜",
            };
        });
    }
    auto prompt = apply(tmpl, inputs.messages, inputs.tools.empty() ? json() : inputs.tools, inputs.add_generation_prompt);

    // Hacks to fix the official (broken) prompt.
    // It is advisable to use --chat-template-file models/templates/llama-cpp-deepseek-r1.jinja instead,
    // until the official template is fixed.
    if (tmpl.source().find("{% if ns.is_tool %}{{'<｜tool▁outputs▁end｜>'}}") != std::string::npos) {
        // Don't leave the chat dangling after tool results
        if (string_ends_with(prompt, "<｜tool▁outputs▁end｜>")) {
            prompt += "<｜end▁of▁sentence｜>";
            if (inputs.add_generation_prompt) {
                prompt += "<｜Assistant｜>";
            }
        }
        // Fix up tool call delta example added by Minja
        prompt = std::regex_replace(
            prompt,
            std::regex("(<｜tool▁call▁end｜>)[\\s\\r\\n]*(<｜tool▁outputs▁begin｜>|<｜User｜>)"),
            "$1<｜tool▁calls▁end｜><｜end▁of▁sentence｜>$2");
    }
    data.prompt = prompt;
    data.format = inputs.extract_reasoning ? COMMON_CHAT_FORMAT_DEEPSEEK_R1_EXTRACT_REASONING : COMMON_CHAT_FORMAT_DEEPSEEK_R1;
    return data;
}
static common_chat_msg handle_think_tag_prelude(const std::string & input, bool is_partial, bool extract_reasoning, const std::function<common_chat_msg(const std::string &)> & rest_parser) {
    std::smatch match;
    // TODO: handle is_partial w/ common_regex
    static const std::regex reasoning_content_regex("((?:<think>)?([\\s\\S\\r\\n]*?)</think>)?([\\s\\S\\r\\n]*)");
    if (std::regex_match(input, match, reasoning_content_regex)) {
        auto rest = match[3].str();
        auto msg = rest_parser(rest);
        auto reasoning_content = string_strip(match[2].str());
        if (extract_reasoning) {
            msg.reasoning_content = reasoning_content;
        } else if (!reasoning_content.empty()) {
            std::ostringstream content;
            content << "<think>" << reasoning_content << "</think>" << msg.content;
            msg.content = content.str();
        }
        return msg;
    }
    return rest_parser(input);
}
static common_chat_msg common_chat_parse_deepseek_r1(const std::string & input, bool is_partial, bool extract_reasoning) {
    return handle_think_tag_prelude(input, is_partial, extract_reasoning, [&](const std::string & input) {
        static const common_regex tool_calls_begin("[\\s\\r\\n]*(?:<｜tool▁calls▁begin｜>|<｜tool_calls_begin｜>|<｜tool calls begin｜>|<｜tool\\\\_calls\\\\_begin｜>)");
        static const common_regex tool_calls_end("<｜tool▁calls▁end｜>");
        static const common_regex function_regex("<｜tool▁call▁begin｜>function<｜tool▁sep｜>([^\n]+)\n```json\n");
        static const common_regex close_regex("```[\\s\\r\\n]*<｜tool▁call▁end｜>");
        static const common_regex reasoning_content_regex("((?:<think>)?([\\s\\S\\r\\n]*?)</think>)?([\\s\\S\\r\\n]*)");
        
        return parse_json_tool_calls(input, is_partial, tool_calls_begin, function_regex, close_regex, tool_calls_end);
    });
}

static common_chat_params common_chat_params_init_firefunction_v2(const common_chat_template & tmpl, const struct templates_params & inputs) {
    LOG_DBG("%s\n", __func__);
    common_chat_params data;
    data.prompt = apply(tmpl, inputs.messages, /* tools= */ nullptr, inputs.add_generation_prompt, {
        {"datetime", "Jan 29 2025 13:00:00 GMT"},
        {"functions", json(inputs.tools.empty() ? "" : inputs.tools.dump(2))},
    });
    if (inputs.tools.is_array() && !inputs.tools.empty()) {
        data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            auto schemas = json::array();
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                schemas.push_back({
                    {"type", "object"},
                    {"properties", {
                        {"name", {
                            {"type", "string"},
                            {"const", function.at("name")},
                        }},
                        {"arguments", function.at("parameters")},
                    }},
                    {"required", json::array({"name", "arguments", "id"})},
                });
            });
            auto schema = json {
                {"type", "array"},
                {"items", schemas.size() == 1 ? schemas[0] : json {{"anyOf", schemas}}},
                {"minItems", 1},
            };
            if (!inputs.parallel_tool_calls) {
                schema["maxItems"] = 1;
            }
            builder.add_rule("root", "\" functools\"? " + builder.add_schema("tool_calls", schema));
        });
        data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, " functools["});
        data.preserved_tokens = {
            " functools[",
        };
        data.format = COMMON_CHAT_FORMAT_FIREFUNCTION_V2;
    } else {
        data.format = COMMON_CHAT_FORMAT_CONTENT_ONLY;
    }
    return data;
}
static common_chat_msg common_chat_parse_firefunction_v2(const std::string & input, bool is_partial) {
    return parse_prefixed_json_tool_call_array(input, is_partial, " functools[", /* rstrip_prefix= */ 1);
}

static common_chat_params common_chat_params_init_functionary_v3_2(const common_chat_template & tmpl, const struct templates_params & inputs) {
    // >>>all\nlet's call functions>>>fn1\n{"arg1": 1...}\n>>>fn2\n{"arg1": 1...}...
    // Using ">>>f1\n", ">>>f2\n"... as trigger words for the grammar
    common_chat_params data;
    data.prompt = apply(tmpl, inputs.messages, inputs.tools.empty() ? json() : inputs.tools, inputs.add_generation_prompt);
    data.format = COMMON_CHAT_FORMAT_FUNCTIONARY_V3_2;
    if (inputs.tools.is_array() && !inputs.tools.empty()) {
        data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            std::vector<std::string> first_tool_rules;
            std::vector<std::string> subsequent_tool_rules;
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");
                builder.resolve_refs(parameters);
                auto args_rule = builder.add_schema(name + "-args", parameters);
                first_tool_rules.push_back(builder.add_rule(name + "-call", "( \"assistant<|end_header_id|>\\n\" )? \"" + name + "\\n\" " + args_rule));
                subsequent_tool_rules.push_back(builder.add_rule(name + "-call2", "\">>>" + name + "\\n\" " + args_rule));
                data.grammar_triggers.push_back({
                    COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_START,
                    regex_escape(name + "\n"),
                });
                data.grammar_triggers.push_back({
                    COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_START,
                    regex_escape("assistant<|end_header_id|>\n" + name + "\n"),
                });
                data.grammar_triggers.push_back({
                    COMMON_GRAMMAR_TRIGGER_TYPE_WORD,
                    regex_escape(">>>" + name + "\n"),
                });
                data.grammar_triggers.push_back({
                    COMMON_GRAMMAR_TRIGGER_TYPE_WORD,
                    ">>>assistant<|end_header_id|>\n" + name,
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
static common_chat_msg common_chat_parse_functionary_v3_2(const std::string & input, bool is_partial) {
    throw std::runtime_error("Functionary v3.2 parsing not implemented yet");
    static const common_regex function_regex(R"((?:>>>)?(?:assistant<|end_header_id|>\n)?(\w+)\n)");
    static const common_regex close_regex(R"($|(?=>>>))");

    std::string content;
    auto it = input.begin();
    const auto end = input.end();

    parse_literal(it, end, "all\n");
    //     auto match = function_regex.search(std::string(it, end));
    //     if (match.type == COMMON_REGEX_MATCH_TYPE_FULL || (is_partial && match.type == COMMON_REGEX_MATCH_TYPE_PARTIAL)) {
            
    //     std::smatch match;
    //     if (std::regex_search(it, end, match, function_regex)) {
    //         auto fun_it = match.prefix().second;
    //         content = std::string(it, fun_it);
    //         it = fun_it;
    //     } else {
    //         common_chat_msg res;
    //         res.role = "assistant";
    //         res.content = std::string(it, end);
    //         return res;
    //     }
    // }
    auto res = parse_json_tool_calls(std::string(it, end), is_partial, std::nullopt, function_regex, close_regex, std::nullopt, /* allow_raw_python= */ true);
    res.content = content + res.content;
    return res;
}

static common_chat_params common_chat_params_init_functionary_v3_1_llama_3_1(const common_chat_template & tmpl, const struct templates_params & inputs) {
    // https://github.com/MeetKai/functionary/blob/main/tests/prompt_test_v3-llama3.1.txt
    common_chat_params data;
    json tools = inputs.tools.is_null() ? inputs.tools : json::array();
    std::string python_code_argument_name;
    auto has_raw_python = false;

    data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;
    data.grammar = build_grammar([&](const common_grammar_builder & builder) {
        std::vector<std::string> tool_rules;
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
            tool_rules.push_back(builder.add_rule(name + "-call", "\"<function=" + name + ">\" " + builder.add_schema(name + "-args", parameters) + " \"</function>\" space"));
        });
        if (has_raw_python) {
            tool_rules.push_back(builder.add_rule("python-call", "\"<|python_tag|>\" .*"));
            data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<|python_tag|>"});
            data.preserved_tokens.push_back("<|python_tag|>");
        }
        auto tool_call = builder.add_rule("tool_call", string_join(tool_rules, " | ")) + " space";
        builder.add_rule("root", inputs.parallel_tool_calls ? "(" + tool_call + ")+" : tool_call);
        data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<function="});
    });

    data.prompt = apply(tmpl, inputs.messages, inputs.tools.empty() ? json() : inputs.tools, inputs.add_generation_prompt);
    // TODO: if (has_raw_python)
    data.format = COMMON_CHAT_FORMAT_FUNCTIONARY_V3_1_LLAMA_3_1;
    return data;
}
static common_chat_msg common_chat_parse_functionary_v3_1_llama_3_1(const std::string & input, bool is_partial) {
    // This version of Functionary still supports the llama 3.1 tool call format for the python tool.
    // TODO: partial regex.
    static const common_regex python_tag_regex(R"(<\|python_tag\|>([\s\S\n]*)$)");

    auto match = python_tag_regex.search(input, /* as_match= */ true);
    if (match.type == COMMON_REGEX_MATCH_TYPE_FULL) {
        auto code = match.groups[1].str();
        common_chat_msg msg;
        msg.role = "assistant";
        // msg.content = std::string(input.begin(), match.groups[0].begin);
        msg.tool_calls.push_back({
            /* .name = */ "python",
            /* .arguments = */ (json {{"code", code}}).dump(),
            /* .id = */ "",
        });
        return msg;
    }
    if (match.type == COMMON_REGEX_MATCH_TYPE_PARTIAL && is_partial) {
        auto prelude = std::string(input.begin(), match.groups[0].begin);
        return {
            .role = "assistant",
            .content = prelude,
            .tool_calls = {},
        };
    }
    static const common_regex function_regex(R"(<function=(\w+)>)");
    static const common_regex close_regex(R"(</function>)");
    return parse_json_tool_calls(input, is_partial, std::nullopt, function_regex, close_regex, std::nullopt);
}

static common_chat_params common_chat_params_init_hermes_2_pro(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;
    // (content)?(<tool_call>{"name": "foo", "arguments": {"a": 1}}</tool_call>)*
    data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;
    data.grammar = build_grammar([&](const common_grammar_builder & builder) {
        std::vector<std::string> tool_rules;
        std::vector<std::string> tool_call_alts;
        foreach_function(inputs.tools, [&](const json & tool) {
            const auto & function = tool.at("function");
            std::string name = function.at("name");
            auto parameters = function.at("parameters");
            builder.resolve_refs(parameters);
            tool_rules.push_back(builder.add_schema(name + "-call", {
                {"type", "object"},
                {"properties", json {
                    {"name", json {{"const", name}}},
                    {"arguments", parameters},
                }},
                {"required", json::array({"name", "arguments"})},
            }));
            tool_call_alts.push_back(builder.add_rule(
                name + "-function-tag",
                "\"<function\" ( \"=" + name + "\" | \" name=\\\"" + name + "\\\"\" ) \">\" space " +
                builder.add_schema(name + "-args", parameters) + " "
                "\"</function>\" space"));

            data.grammar_triggers.push_back({
                COMMON_GRAMMAR_TRIGGER_TYPE_WORD,
                "<function=" + name + ">",
            });
            auto escaped_name = regex_escape(name);
            data.grammar_triggers.push_back({
                COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN,
                "<function\\s+name\\s*=\\s*\"" + escaped_name + "\"",
            });
        });
        auto any_tool_call = builder.add_rule("any_tool_call", "( " + string_join(tool_rules, " | ") + " ) space");
        std::vector<std::string> alt_tags {
            any_tool_call,
            "\"<tool_call>\" space "     + any_tool_call + " \"</tool_call>\"",
            // The rest is just to accommodate common "good bad" outputs.
            "\"<function_call>\" space " + any_tool_call + " \"</function_call>\"",
            "\"<response>\"  space "     + any_tool_call + " \"</response>\"",
            "\"<tools>\"     space "     + any_tool_call + " \"</tools>\"",
            "\"<json>\"      space "     + any_tool_call + " \"</json>\"",
            "\"<xml>\"      space "     + any_tool_call + " \"</xml>\"",
            "\"<JSON>\"      space "     + any_tool_call + " \"</JSON>\"",
        };
        auto wrappable_tool_call = builder.add_rule("wrappable_tool_call", "( " + string_join(alt_tags, " | ") + " ) space");
        tool_call_alts.push_back(wrappable_tool_call);
        tool_call_alts.push_back(
            "( \"```\\n\" | \"```json\\n\" | \"```xml\\n\" ) space " + wrappable_tool_call + " space \"```\" space ");
        auto tool_call = builder.add_rule("tool_call", string_join(tool_call_alts, " | "));
        builder.add_rule("root", inputs.parallel_tool_calls ? "(" + tool_call + ")+" : tool_call);
        data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<tool_call>"});
        data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<function"});
        // Trigger on some common known "good bad" outputs (only from the start and with a json that's about a specific argument name to avoid false positives)
        data.grammar_triggers.push_back({
            COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_START,
            "(?:```(?:json|xml)?\n\\s*)?(?:<function_call>|<tools>|<xml><json>|<response>)?\\s*\\{\\s*\"", //name\"\\s*:\\s*\"" + escaped_name + "\"",
        });
        data.preserved_tokens = {
            "<think>",
            "</think>",
            "<tool_call>",
            "</tool_call>",
            "<function",
            "<tools>",
            "</tools>",
            "<response>",
            "</response>",
            "<function_call>",
            "</function_call>",
            "<json>",
            "</json>",
            "<JSON>",
            "</JSON>",
            "```",
            "```json",
            "```xml",
        };
    });

    data.prompt = apply(tmpl, inputs.messages, inputs.tools.empty() ? json() : inputs.tools, inputs.add_generation_prompt);
    data.format = inputs.extract_reasoning ? COMMON_CHAT_FORMAT_HERMES_2_PRO_EXTRACT_REASONING : COMMON_CHAT_FORMAT_HERMES_2_PRO;
    return data;
}
static common_chat_msg common_chat_parse_hermes_2_pro(const std::string& input, bool is_partial, bool extract_reasoning) {
    return handle_think_tag_prelude(input, is_partial, extract_reasoning, [is_partial](const std::string & input) {
        static const common_regex open_regex(
            "(?:"
            "(```(?:xml|json)?\\n\\s*)?"         // match 1 (block_start)
            "(<tool_call>"                   // match 2 (open_tag)
            "|<function_call>"
            "|<tool>"
            "|<tools>"
            "|<response>"
            "|<json>"
            "|<xml>"
            "|<JSON>"
            ")?"
            "(\\s*\\{\\s*\"name\"\\s*:[\\s\\S]*)"    // match 3 (named tool call + rest)
            ")"
            "|"
            "(?:<function=([^>]+)>"            // match 4 (function name)
            "|<function name=\"([^\"]+)\">)" // match 5 (function name again)
            "([\\s\\S]*)",                   // match 6 (function arguments + rest)})"
            /* at_start= */ true
        );

        common_chat_msg msg;
        msg.role = "assistant";

        std::string::const_iterator it = input.begin();
        const std::string::const_iterator end = input.end();

        // while (it != end) {
        do {
            auto match = open_regex.search(it, end, /* as_match= */ true);
            if (match.type == COMMON_REGEX_MATCH_TYPE_FULL) {
                msg.content += std::string(it, match.groups[0].begin);

                const auto & block_start = match.groups[1];
                std::string block_end = block_start.empty() ? "" : "```";

                const auto & open_tag = match.groups[2];
                std::string close_tag;

                if (!match.groups[3].empty()) {
                    close_tag = open_tag.empty() ? "" : "</" + open_tag.str().substr(1);
                    auto json_it = match.groups[3].begin;
                    common_json partial_tool_call;
                    common_chat_tool_call tool_call;
                    if (parse_json_with_arguments(json_it, end, is_partial, [](const std::vector<std::string> & path) { return path.size() == 1 && path[0] == "arguments"; }, partial_tool_call) &&
                        process_tool_call(partial_tool_call.json, partial_tool_call, tool_call))
                    {
                        msg.tool_calls.emplace_back(std::move(tool_call));
                        it = json_it;  // Move iterator past parsed JSON

                        if (!is_partial) {
                            // Handle close tags
                            consume_spaces(it, end);
                            if (!close_tag.empty() && !parse_literal(it, end, close_tag)) {
                                if (is_partial && string_find_partial_stop(std::string(it, end), close_tag)) {
                                    break;
                                }
                                throw std::runtime_error("Failed to parse closing tag");
                            }
                            consume_spaces(it, end);
                            if (!block_end.empty() && !parse_literal(it, end, block_end)) {
                                if (is_partial && string_find_partial_stop(std::string(it, end), block_end)) {
                                    break;
                                }
                                throw std::runtime_error("Failed to parse block end");
                            }
                            consume_spaces(it, end);

                        }
                    } else if (is_partial) {
                        break;
                    } else {
                        // Not a valid tool call, treat as content
                        msg.content += match.groups[0].str();//std::string(match[0].first, match[0].second);
                        it = match.groups[0].end;
                    }
                } else {
                    auto function_name = match.groups[4].str();
                    if (function_name.empty()) {
                        function_name = match.groups[5].str();
                    }
                    GGML_ASSERT(!function_name.empty());

                    close_tag = "</function>";

                    // Start parsing from after the opening tags
                    auto json_it = match.groups[6].begin;
                    common_json arguments;
                    if (parse_json_with_arguments(json_it, end, is_partial, [](const std::vector<std::string> & path) { return path.empty(); }, arguments)) {
                        it = json_it;  // Move iterator past parsed JSON

                        common_chat_tool_call tool_call;
                        if (process_tool_call(function_name, "", arguments.json.dump(), arguments, tool_call)) {
                            msg.tool_calls.emplace_back(tool_call);

                            // Handle close tags
                            consume_spaces(it, end);
                            if (!close_tag.empty() && !parse_literal(it, end, close_tag)) {
                                if (is_partial && string_find_partial_stop(std::string(it, end), close_tag)) {
                                    break;
                                }
                                throw std::runtime_error("Failed to parse closing tag");
                            }
                            consume_spaces(it, end);
                            if (!block_end.empty() && !parse_literal(it, end, block_end)) {
                                if (is_partial && string_find_partial_stop(std::string(it, end), block_end)) {
                                    break;
                                }
                                throw std::runtime_error("Failed to parse block end");
                            }
                            consume_spaces(it, end);
                        } else {
                            // Partial, whatever.  
                        }

                    } else {
                        // Not a valid tool call, treat as content
                        msg.content += match.groups[0].str();//std::string(match[0].first, match[0].second);
                        it = match.groups[0].end;
                    }
                }
            } else if (match.type == COMMON_REGEX_MATCH_TYPE_PARTIAL && is_partial) {
                // Partially matched opening regex in partial mode, skipping until there's more data.
                break;
            } else {
                // Add remaining content
                msg.content += std::string(it, end);
                break;
            }
        }
        while (false);
        return msg;
    });
}

static common_chat_params common_chat_params_init_without_tools(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;
    data.prompt = apply(tmpl, inputs.messages, inputs.tools.empty() ? json() : inputs.tools, inputs.add_generation_prompt);
    data.format = COMMON_CHAT_FORMAT_CONTENT_ONLY;
    data.grammar_lazy = false;
    if (!inputs.json_schema.is_null()) {
        if (!inputs.grammar.empty()) {
            throw std::runtime_error("Either \"json_schema\" or \"grammar\" can be specified, but not both");
        }
        data.grammar = json_schema_to_grammar(inputs.json_schema);
    } else {
        data.grammar = inputs.grammar;
    }
    return data;
}

static common_chat_params common_chat_templates_apply_jinja(
    const struct common_chat_templates * tmpls,
    const struct common_chat_templates_inputs & inputs)
{
    templates_params params;
    params.tools = common_chat_tools_to_json_oaicompat<json>(inputs.tools);
    const auto & tmpl = params.tools.is_array() && tmpls->template_tool_use
        ? *tmpls->template_tool_use
        : *tmpls->template_default;
    const auto & src = tmpl.source();
    const auto & caps = tmpl.original_caps();
    params.messages = common_chat_msgs_to_json_oaicompat<json>(inputs.messages, /* concat_text= */ !tmpl.original_caps().requires_typed_content);
    params.add_generation_prompt = inputs.add_generation_prompt;
    params.extract_reasoning = inputs.extract_reasoning;
    params.tool_choice = inputs.tool_choice;
    params.grammar = inputs.grammar;
    if (!inputs.json_schema.empty()) {
        params.json_schema = json::parse(inputs.json_schema);
    }

    if (inputs.parallel_tool_calls && !tmpl.original_caps().supports_parallel_tool_calls) {
        LOG_DBG("Disabling parallel_tool_calls because the template does not support it\n");
        params.parallel_tool_calls = false;
    } else {
        params.parallel_tool_calls = inputs.parallel_tool_calls;
    }

    if (params.tools.is_array()) {
        if (params.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE && !params.grammar.empty()) {
            throw std::runtime_error("Cannot specify grammar with tools");
        }
        if (caps.supports_tool_calls && !caps.supports_tools) {
            LOG_WRN("Template supports tool calls but does not natively describe tools. The fallback behaviour used may produce bad results, inspect prompt w/ --verbose & consider overriding the template.\n");
        }
    }

    // DeepSeek R1: use handler in all cases except json schema (thinking / tools).
    if (src.find("<｜tool▁calls▁begin｜>") != std::string::npos && params.json_schema.is_null()) {
        return common_chat_params_init_deepseek_r1(tmpl, params);
    }

    // Command R7B: : use handler in all cases except json schema (thinking / tools).
    if (src.find("<|END_THINKING|><|START_ACTION|>") != std::string::npos && params.json_schema.is_null()) {
        return common_chat_params_init_command_r7b(tmpl, params);
    }

    // Hermes 2/3 Pro, Qwen 2.5 Instruct (w/ tools)
    if (src.find("<tool_call>") != std::string::npos && params.json_schema.is_null()) {
        return common_chat_params_init_hermes_2_pro(tmpl, params);
    }

    // Use generic handler when mixing tools + JSON schema.
    // TODO: support that mix in handlers below.
    if ((params.tools.is_array() && params.json_schema.is_object())) {
        return common_chat_params_init_generic(tmpl, params);
    }

    // Functionary prepends "all\n" to plain content outputs, so we use its handler in all cases.
    if (src.find(">>>all") != std::string::npos) {
        return common_chat_params_init_functionary_v3_2(tmpl, params);
    }

    // Firefunction v2 requires datetime and functions in the context even w/o tools, so we also use its handler in all cases.
    if (src.find(" functools[") != std::string::npos) {
        return common_chat_params_init_firefunction_v2(tmpl, params);
    }

    // Plain handler (no tools)
    if (params.tools.is_null() || inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_NONE) {
        return common_chat_params_init_without_tools(tmpl, params);
    }

    // Functionary v3.1 (w/ tools)
    if (src.find("<|start_header_id|>") != std::string::npos
        && src.find("<function=") != std::string::npos) {
        return common_chat_params_init_functionary_v3_1_llama_3_1(tmpl, params);
    }

    // Llama 3.1, 3.2, 3.3 (w/ tools)
    if (src.find("<|start_header_id|>ipython<|end_header_id|>") != std::string::npos) {
        auto allow_python_tag_builtin_tools = src.find("<|python_tag|>") != std::string::npos;
        return common_chat_params_init_llama_3_1_tool_calls(tmpl, params, allow_python_tag_builtin_tools);
    }

    // Mistral Nemo (w/ tools)
    if (src.find("[TOOL_CALLS]") != std::string::npos) {
        return common_chat_params_init_mistral_nemo(tmpl, params);
    }

    // Generic fallback
    return common_chat_params_init_generic(tmpl, params);
}

// Legacy template route (adhoc C++ implementation of known templates), forward to llama_chat_apply_template.
static common_chat_params common_chat_templates_apply_legacy(
    const struct common_chat_templates * tmpls,
    const struct common_chat_templates_inputs & inputs)
{
    int alloc_size = 0;
    std::vector<llama_chat_message> chat;
    std::vector<std::string> contents;
    for (const auto & msg : inputs.messages) {
        auto content = msg.content;
        for (const auto & part : msg.content_parts) {
            if (part.type != "text") {
                LOG_WRN("Ignoring non-text content part: %s\n", part.type.c_str());
                continue;
            }
            if (!content.empty()) {
                content += "\n";;
            }
            content += part.text;
        }
        contents.emplace_back(std::move(content));
    }
    for (size_t i = 0; i < contents.size(); ++i) {
        const auto & msg = inputs.messages[i];
        const auto & content = contents[i];
        chat.push_back({msg.role.c_str(), content.c_str()});
        alloc_size += (msg.role.size() + content.size()) * 1.25;
    }

    std::vector<char> buf(alloc_size);

    // run the first time to get the total output length
    const auto & src = tmpls->template_default->source();
    int32_t res = llama_chat_apply_template(src.c_str(), chat.data(), chat.size(), inputs.add_generation_prompt, buf.data(), buf.size());

    // error: chat template is not supported
    if (res < 0) {
        // if the custom "tmpl" is not supported, we throw an error
        // this is a bit redundant (for good), since we're not sure if user validated the custom template with llama_chat_verify_template()
        throw std::runtime_error("this custom template is not supported");
    }

    // if it turns out that our buffer is too small, we resize it
    if ((size_t) res > buf.size()) {
        buf.resize(res);
        res = llama_chat_apply_template(src.c_str(), chat.data(), chat.size(), inputs.add_generation_prompt, buf.data(), buf.size());
    }

    common_chat_params params;
    params.prompt = std::string(buf.data(), res);
    if (!inputs.json_schema.empty()) {
        params.grammar = json_schema_to_grammar(json::parse(inputs.json_schema));
    } else {
        params.grammar = inputs.grammar;
    }
    return params;
}

common_chat_params common_chat_templates_apply(
    const struct common_chat_templates * tmpls,
    const struct common_chat_templates_inputs & inputs)
{
    GGML_ASSERT(tmpls != nullptr);
    return inputs.use_jinja
        ? common_chat_templates_apply_jinja(tmpls, inputs)
        : common_chat_templates_apply_legacy(tmpls, inputs);
}

static common_chat_msg common_chat_parse_content_only(const std::string & input) {
    common_chat_msg msg;
    msg.role = "assistant";
    msg.content = input;
    return msg;
}

static common_chat_msg common_chat_parse(common_chat_format format, const std::string & input, bool is_partial) {
    LOG_DBG("Parsing input with format %s:\n%s\n", common_chat_format_name(format).c_str(), input.c_str());
    switch (format) {
        case COMMON_CHAT_FORMAT_CONTENT_ONLY:
            return common_chat_parse_content_only(input);
        case COMMON_CHAT_FORMAT_GENERIC:
            return common_chat_parse_generic(input, is_partial);
        case COMMON_CHAT_FORMAT_MISTRAL_NEMO:
            return common_chat_parse_mistral_nemo(input, is_partial);
        case COMMON_CHAT_FORMAT_LLAMA_3_X:
            return common_chat_parse_llama_3_1(input, is_partial);
        case COMMON_CHAT_FORMAT_LLAMA_3_X_WITH_BUILTIN_TOOLS:
            return common_chat_parse_llama_3_1(input, is_partial, /* with_builtin_tools= */ true);
        case COMMON_CHAT_FORMAT_DEEPSEEK_R1:
            return common_chat_parse_deepseek_r1(input, is_partial, /* extract_reasoning= */ false);
        case COMMON_CHAT_FORMAT_DEEPSEEK_R1_EXTRACT_REASONING:
            return common_chat_parse_deepseek_r1(input, is_partial, /* extract_reasoning= */ true);
        case COMMON_CHAT_FORMAT_FUNCTIONARY_V3_2:
            return common_chat_parse_functionary_v3_2(input, is_partial);
        case COMMON_CHAT_FORMAT_FUNCTIONARY_V3_1_LLAMA_3_1:
            return common_chat_parse_functionary_v3_1_llama_3_1(input, is_partial);
        case COMMON_CHAT_FORMAT_HERMES_2_PRO:
            return common_chat_parse_hermes_2_pro(input, is_partial, /* extract_reasoning= */ false);
        case COMMON_CHAT_FORMAT_HERMES_2_PRO_EXTRACT_REASONING:
            return common_chat_parse_hermes_2_pro(input, is_partial, /* extract_reasoning= */ true);
        case COMMON_CHAT_FORMAT_FIREFUNCTION_V2:
            return common_chat_parse_firefunction_v2(input, is_partial);
        case COMMON_CHAT_FORMAT_COMMAND_R7B:
            return common_chat_parse_command_r7b(input, is_partial, /* extract_reasoning= */ false);
        case COMMON_CHAT_FORMAT_COMMAND_R7B_EXTRACT_REASONING:
            return common_chat_parse_command_r7b(input, is_partial, /* extract_reasoning= */ true);
        default:
            throw std::runtime_error("Unsupported format: " + common_chat_format_name(format));
    }
}

std::optional<common_chat_msg> common_chat_parse(const std::string & input, bool is_partial, common_chat_format format, const std::vector<common_regex> & trigger_regexes) {
    if (is_partial) {
        bool found_trigger = false;
        auto earliest_partial_trigger = input.end();

        for (const auto & trigger_regex : trigger_regexes) {
            auto match = trigger_regex.search(input);
            if (match.type == COMMON_REGEX_MATCH_TYPE_PARTIAL) {
                earliest_partial_trigger = std::min(earliest_partial_trigger, match.groups[0].begin);
            } else if (match.type == COMMON_REGEX_MATCH_TYPE_FULL) {
                if (match.groups[0].begin < earliest_partial_trigger) {
                    found_trigger = true;
                    break;
                }
            }
        }

        if (!found_trigger && earliest_partial_trigger != input.end()) {
            // Stop stopping at the earliest partial trigger to avoid messing the parsing big time.
            try {
                auto before_trigger = std::string(input.begin(), earliest_partial_trigger);
                if (before_trigger.empty()) {
                    return std::nullopt;
                }
                auto parsed = common_chat_parse(format, before_trigger, /* is_partial= */ true);
                return parsed;
            } catch (const std::exception &) {
                return std::nullopt;
            }
        }
    }

    // try {
        auto parsed = common_chat_parse(format, input, is_partial);
        if (parsed.empty()) {
            return std::nullopt;
        }
        return parsed;
    // } catch (const std::exception & ex) {
    //     LOG_WRN("Failed to parse chat message (is_partial = %s): %s\n", is_partial ? "true" : "false", ex.what());
    //     return std::nullopt;
    // }
}