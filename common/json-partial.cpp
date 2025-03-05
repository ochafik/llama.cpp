#include "common.h"
#include <json-partial.h>
#include <variant>
#include <optional>

#include <json.hpp>

using json = nlohmann::ordered_json;

common_json_healed common_json::heal(const std::string & magic) const {
    std::string healed_source;
    std::string actual_magic;
    auto flags = this->flags;

    auto move_out = [](int flags) {
        if (flags & COMMON_JSON_FLAGS_DICT_INSIDE_KEY) {
            flags &= ~COMMON_JSON_FLAGS_DICT_INSIDE_KEY;
            flags |=  COMMON_JSON_FLAGS_DICT_AFTER_KEY;
        } else if (flags & COMMON_JSON_FLAGS_DICT_INSIDE_VALUE) {
            flags &= ~COMMON_JSON_FLAGS_DICT_INSIDE_VALUE;
            flags |=  COMMON_JSON_FLAGS_DICT_AFTER_VALUE;
        } else if (flags & COMMON_JSON_FLAGS_ARRAY_INSIDE_VALUE) {
            flags &= ~COMMON_JSON_FLAGS_ARRAY_INSIDE_VALUE;
            flags |=  COMMON_JSON_FLAGS_ARRAY_AFTER_VALUE;
        } else {
            throw std::runtime_error("Cannot move out of a location that is not inside a key, value or array value");
        }
        return flags;
    };
    if (flags & COMMON_JSON_FLAGS_VALUE_INSIDE_STRING) {
        healed_source = truncated_source + magic + "\"";
        actual_magic = magic;
        flags &= ~COMMON_JSON_FLAGS_VALUE_INSIDE_STRING;
        flags = move_out(flags);
    } else if (flags & COMMON_JSON_FLAGS_VALUE_INSIDE_STRING_AFTER_ESCAPE) {
        GGML_ASSERT(string_ends_with(truncated_source, "\\"));
        healed_source = truncated_source.substr(0, truncated_source.size() - 1) + magic + "\"";
        actual_magic = magic;
        flags &= ~COMMON_JSON_FLAGS_VALUE_INSIDE_STRING_AFTER_ESCAPE;
        flags = move_out(flags);
    } else if (flags & COMMON_JSON_FLAGS_VALUE_INSIDE_IDENT) {
        // TODO: move back out of the identifier, or complete it
        throw std::runtime_error("Cannot heal a truncated JSON that stopped inside a keyword / identifier");
    } else {
        healed_source = truncated_source;
    }
    
    if (flags & COMMON_JSON_FLAGS_DICT_BEFORE_KEY) {
        if (actual_magic.empty()) {
            healed_source += "\"" + magic + "\": null";
            actual_magic = "\"" + magic;
        } else {
            auto str = string_strip(healed_source);
            if (str.back() == ',') {
                healed_source += " \"\": null";
            } else if (str.back() != '{') {
                throw std::runtime_error("Cannot heal a truncated JSON that stopped in an unknown location");
            }
        }
    } else if (flags & COMMON_JSON_FLAGS_DICT_AFTER_KEY) {
        if (actual_magic.empty()) {
            healed_source += ": \"" + magic + "\"";
            actual_magic = ": \"" + magic;
        } else {
            healed_source += ": null";
        }
    } else if (flags & COMMON_JSON_FLAGS_DICT_BEFORE_VALUE) {
        if (actual_magic.empty()) {
            healed_source += "\"" + magic + "\"";
            actual_magic = "\"" + magic;
        } else {
            healed_source += "null";
        }
    } else if (flags & COMMON_JSON_FLAGS_DICT_AFTER_VALUE) {
        if (actual_magic.empty()) {
            healed_source += ", \"" + magic + "\": null";
            actual_magic = ", \"" + magic;
        }
    } else if (flags & COMMON_JSON_FLAGS_ARRAY_BEFORE_VALUE) {
        if (actual_magic.empty()) {
            healed_source += "\"" + magic + "\"";
            actual_magic = "\"" + magic;
        } else {
            auto str = string_strip(healed_source);
            if (str.back() == ',') {
                healed_source += "\"\"";
            } else if (str.back() != '[') {
                throw std::runtime_error("Cannot heal a truncated JSON that stopped in an unknown location");
            }
        }
    } else if (flags & COMMON_JSON_FLAGS_ARRAY_AFTER_VALUE) {
        if (actual_magic.empty()) {
            healed_source += ", \"" + magic + "\"";
            actual_magic = ", \"" + magic;
        }
    }

    healed_source += nesting_closure;

    common_json_healed res;
    res.json = json::parse(healed_source);
    res.magic = actual_magic;
    return res;
}

std::variant<std::nullopt_t, nlohmann::ordered_json, common_json> common_json::parse(const std::string & str) {
    auto it = str.begin();
    const auto end = str.end();
    return parse(it, end);
}

std::variant<std::nullopt_t, nlohmann::ordered_json, common_json> common_json::parse(
    std::string::const_iterator & it,
    const std::string::const_iterator & end)
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
        bool null() override { return true; } // NOLINT
        bool boolean(bool) override { return true; } // NOLINT
        bool number_integer(number_integer_t) override { return true; } // NOLINT
        bool number_unsigned(number_unsigned_t) override { return true; } // NOLINT
        bool number_float(number_float_t, const string_t &) override { return true; } // NOLINT
        bool string(string_t &) override { return true; } // NOLINT
        bool binary(binary_t &) override { return true; } // NOLINT
        bool start_object(std::size_t) override { // NOLINT
            closing_stack.push_back("}");
            // name_stack.emplace_back(std::nullopt);
            return true;
        }
        bool end_object() override { 
            GGML_ASSERT(closing_stack.back() == "}");
            closing_stack.pop_back();
            // name_stack.pop_back();
            return true;
        }
        bool key(string_t &) override { // NOLINT
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
            return true;
        }
    };
    json_error_locator err_loc;
    json::sax_parse(it, end, &err_loc);

    std::string::const_iterator temptative_end;
    if (err_loc.found_error) {
        // std::cerr << "Error at position " << err_loc.position << ":\n";
        // std::cerr << "   Exception: " << err_loc.exception_message << '\n';
        // std::cerr << "  Last token: " << err_loc.last_token << '\n';
        // std::vector<std::string> closing_stack(err_loc.closing_stack.rbegin(), err_loc.closing_stack.rend());
        // std::cerr << "     Closing: " << string_join(closing_stack, "") << '\n';
        temptative_end = it + err_loc.position;
    } else {
        temptative_end = end;
    }
    std::string json_sub {it, temptative_end};
    try {
        auto out = json::parse(json_sub);
        it = temptative_end;
        return out;
    } catch (const std::exception &) {
        return std::nullopt;
    }
}
