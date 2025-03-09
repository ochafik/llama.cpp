#include "common.h"
#include <json-partial.h>
#include <variant>
#include <optional>

#include <json.hpp>

using json = nlohmann::ordered_json;

bool common_json_parse(
    std::string::const_iterator & it,
    const std::string::const_iterator & end,
    bool allow_healing,
    common_json & out)
{
    // // https://json.nlohmann.me/features/parsing/sax_interface/
    struct json_error_locator : public nlohmann::json_sax<json> {
        std::size_t position;
        bool found_error;
        std::string last_token;
        std::string exception_message;
        std::vector<std::optional<std::string>> name_stack;
        std::vector<std::string> closing_stack;

        json_error_locator() : position(0), found_error(false) {}

        bool parse_error(std::size_t position, const std::string & last_token, const json::exception & ex) override { // NOLINT
            this->position = position - 1;
            this->found_error = true;
            this->last_token = last_token;
            this->exception_message = ex.what();
            return false;
        }
        void close_value() {
            if (!closing_stack.empty() && closing_stack.back().empty()) {
                closing_stack.pop_back();
            }
        }
        bool null() override { // NOLINT
            close_value();
            return true;
        }
        bool boolean(bool) override { // NOLINT
            close_value();
            return true;
        }
        bool number_integer(number_integer_t) override { // NOLINT
            close_value();
            return true;
        }
        bool number_unsigned(number_unsigned_t) override { // NOLINT
            close_value();
            return true;
        }
        bool number_float(number_float_t, const string_t &) override { // NOLINT
            close_value();
            return true;
        }
        bool string(string_t &) override { // NOLINT
            close_value();
            return true;
        }
        bool binary(binary_t &) override { // NOLINT
            close_value();
            return true;
        }
        bool start_object(std::size_t) override { // NOLINT
            closing_stack.push_back("}");
            // name_stack.emplace_back(std::nullopt);
            return true;
        }
        bool end_object() override { 
            GGML_ASSERT(closing_stack.back() == "}");
            closing_stack.pop_back();
            // name_stack.pop_back();
            close_value();
            return true;
        }
        bool key(string_t &) override { // NOLINT
            closing_stack.emplace_back("");
            // name_stack.back() = key;
            return true;
        }
        bool start_array(std::size_t) override { // NOLINT
            closing_stack.push_back("]");
            // name_stack.emplace_back(std::nullopt);
            return true;
        }
        bool end_array() override {
            GGML_ASSERT(closing_stack.back() == "]");
            closing_stack.pop_back();
            // name_stack.pop_back();
            close_value();
            return true;
        }
    };
    json_error_locator err_loc;
    json::sax_parse(it, end, &err_loc);

    std::string::const_iterator temptative_end;
    if (err_loc.found_error) {
        temptative_end = it + err_loc.position;
        fprintf(stderr, "Error at position %zu (is_end = %s): %s\n", err_loc.position, temptative_end == end ? "true" : "false", err_loc.exception_message.c_str());

        auto can_parse = [](const std::string & str) {
            try {
                json::parse(str); // NOLINT
                return true;
            } catch (const std::exception &) {
                return false;
            }
        };
        if (allow_healing && !err_loc.closing_stack.empty()) {
            std::string str(it, temptative_end);
            auto last_non_sp_pos = str.find_last_not_of(" \n\r\t");
            if (last_non_sp_pos == std::string::npos) {
                throw std::runtime_error("Cannot heal a truncated JSON that stopped in an unknown location");
            }
            auto last_non_sp_char = str[last_non_sp_pos];

            auto rstack = err_loc.closing_stack;
            std::reverse(rstack.begin(), rstack.end());
            auto closing = string_join(rstack, "");
            fprintf(stderr, "Closing: '%s'\n", closing.c_str());

            const auto & magic_seed = out.healing_marker = "$llama.cpp.json$";
            
            if (err_loc.closing_stack.back().empty()) {
                // We're inside an object value
                if (last_non_sp_char == ':') {
                    fprintf(stderr, "Was about to create an object value\n");
                    str += (out.json_healing_marker = "\"" + magic_seed) + "\"" + closing;
                } else if (can_parse(str + ": 1" + closing)) {
                    str += (out.json_healing_marker = ":\"" + magic_seed) + "\"" + closing;
                } else if (last_non_sp_char == '{') {
                    fprintf(stderr, "Was about to create an object\n");
                    str += (out.json_healing_marker = "\"" + magic_seed) + "\": 1" + closing;
                } else if (can_parse(str + "\"" + closing)) {
                    fprintf(stderr, "Was inside an object value string\n");
                    str += (out.json_healing_marker = magic_seed) + "\"" + closing;
                } else if (str[str.length() - 1] == '\\' && can_parse(str + "\\\"" + closing)) {
                    fprintf(stderr, "Was inside an object value string after an escape\n");
                    str += (out.json_healing_marker = "\\" + magic_seed) + "\"" + closing;
                } else {
                    // find last :
                    auto last_pos = str.find_last_of(':');
                    if (last_pos == std::string::npos) {
                        throw std::runtime_error("Cannot heal a truncated JSON that stopped in an unknown location");
                    }
                    fprintf(stderr, "Cutting back to opening : for object value\n");
                    str = str.substr(0, last_pos + 1) + (out.json_healing_marker = "\\" + magic_seed) + "\": 1" + closing;
                }
            } else if (err_loc.closing_stack.back() == "]") {
                if (last_non_sp_char == ',' || last_non_sp_char == '[') {
                    fprintf(stderr, "Was about to create an array value\n");
                    str += (out.json_healing_marker = "\"" + magic_seed) + "\"" + closing;
                } else if (can_parse(str + "\"" + closing)) {
                    fprintf(stderr, "Was inside an array value string\n");
                    str += (out.json_healing_marker = magic_seed) + "\"" + closing;
                } else if (str[str.length() - 1] == '\\' && can_parse(str + "\\\"" + closing)) {
                    fprintf(stderr, "Was inside an array value string after an escape\n");
                    str += (out.json_healing_marker = "\\" + magic_seed) + "\"" + closing;
                } else {
                    auto last_pos = str.find_last_of("[,");
                    if (last_pos == std::string::npos) {
                        throw std::runtime_error("Cannot heal a truncated JSON array stopped in an unknown location");
                    }
                    fprintf(stderr, "Cutting back to last [ or , for array value\n");
                    str = str.substr(0, last_pos + 1) + (out.json_healing_marker = "\"" + magic_seed) + "\"" + closing;
                }
            } else if (err_loc.closing_stack.back() == "}") {
                if (last_non_sp_char == ',' || last_non_sp_char == '{') {
                    fprintf(stderr, "Was about to create an object key+value\n");
                    str += (out.json_healing_marker = "\"" + magic_seed) + "\": 1" + closing;
                } else if (can_parse(str + ",\"\": 1" + closing)) {
                    fprintf(stderr, "Was about to create an object key+value\n");
                    str += (out.json_healing_marker = ",\"" + magic_seed) + "\": 1" + closing;
                } else if (can_parse(str + "\": 1" + closing)) {
                    fprintf(stderr, "Was inside an object key string\n");
                    str += (out.json_healing_marker = magic_seed) + "\": 1" + closing;
                } else if (str[str.length() - 1] == '\\' && can_parse(str + "\\\"" + closing)) {
                    fprintf(stderr, "Was inside an object key string after an escape\n");
                    str += (out.json_healing_marker = "\\" + magic_seed) + "\": 1" + closing;
                } else {
                    auto last_pos = str.find_last_of(':');
                    if (last_pos == std::string::npos) {
                        throw std::runtime_error("Cannot heal a truncated JSON object stopped in an unknown location");
                    }
                    fprintf(stderr, "Cutting back to last : for object key+value\n");
                    str = str.substr(0, last_pos + 1) + (out.json_healing_marker = "\"" + magic_seed) + "\"" + closing;
                    // throw std::runtime_error("Cannot heal a truncated JSON object stopped in an unknown location");
                }
            } else {
                throw std::runtime_error("Cannot heal a truncated JSON object stopped in an unknown location");
            }
            fprintf(stderr, "HEALED:\nSTRING <<<\n%s\n>>>\n\nmagic_cut: <<<\n%s\n>>>\n\n", str.c_str(), out.json_healing_marker.c_str());
            out.json = json::parse(str);
            it = temptative_end;
            return true;
        }
        // TODO: handle unclosed top-level primitive if the stack was empty but we got an error (e.g. "tru", "\"", etc...)
        fprintf(stderr, "Closing: TODO\n"); 
    } else {
        temptative_end = end;
    }
    // std::string json_sub {it, temptative_end};
    try {
        out.json = json::parse(it, temptative_end);
        it = temptative_end;
        return true;
    } catch (const std::exception &) {
        return false;
    }
}
