#include <json.hpp>
#include <variant>
#include <optional>

struct common_json_healed {
    nlohmann::ordered_json json;
    std::string magic;
};

// Flags capture the context of the innermost enclosing array OR object, and of the value we may be in the middle of
// flags: before value, after value, inside string, after string escape
//        before dict key/value, after dict key, before dict value, after dict value
//        before array value, after array value
// tr 0 ue
// 
// 0 { tru 1 "...2...\3..." 4 : 5 "...6...\7..." 8 , 1 ... }
//   [ 10 ""]
enum common_json_flags {
    COMMON_JSON_FLAGS_VALUE_INSIDE_IDENT = 1 << 0,               //       tr|ue
    COMMON_JSON_FLAGS_VALUE_INSIDE_STRING = 1 << 1,              //      "..|.."
    COMMON_JSON_FLAGS_VALUE_INSIDE_STRING_AFTER_ESCAPE = 1 << 2, //     "..\|.."
    COMMON_JSON_FLAGS_DICT_BEFORE_KEY = 1 << 3,                  //       { | ...}
    COMMON_JSON_FLAGS_DICT_INSIDE_KEY = 1 << 4,                  //   { "...|..." : ...}
    COMMON_JSON_FLAGS_DICT_AFTER_KEY = 1 << 5,                   // { "..." | : ...}
    COMMON_JSON_FLAGS_DICT_BEFORE_VALUE = 1 << 6,        //       { "..." : | ...}
    COMMON_JSON_FLAGS_DICT_INSIDE_VALUE = 1 << 7,        //   { "..." : "...|..." }
    COMMON_JSON_FLAGS_DICT_AFTER_VALUE = 1 << 8,         // { "..." : "..." | ...}
    COMMON_JSON_FLAGS_ARRAY_BEFORE_VALUE = 1 << 9,               //       [ | ...]
    COMMON_JSON_FLAGS_ARRAY_INSIDE_VALUE = 1 << 10,              //    [ ...|... ]
    COMMON_JSON_FLAGS_ARRAY_AFTER_VALUE = 1 << 11,               //   [ ... | ]
};

struct common_json {
    int flags;
    std::string truncated_source;
    std::string nesting_closure;
    std::vector<std::optional<std::string>> name_stack;

    /*
        Heals a truncated JSON string with a magic string, returning the healed JSON string and the updated magic string to look for.
        This can be used to heal a JSON, transform its values, then serialize them and truncating them at the updated magic string.
        (for instance many tool call syntaxes involve expressing function arguments as JSON objects, but are streamed back encoded as partial JSON strings)
        
        TODO: pick magic automagically (increment some random string until it's not in the source, can do in one linear pass
        TODO: check that a long json string can be healed from any truncation point (heal then jsonified then truncated at magic should be the same as the original truncation, except for keywords and string escapes)
    */
    common_json_healed heal(const std::string & magic) const;

    static std::variant<std::nullopt_t, nlohmann::ordered_json, common_json> parse(const std::string & str);

    static std::variant<std::nullopt_t, nlohmann::ordered_json, common_json> parse(
        std::string::const_iterator & it,
        const std::string::const_iterator & end);
};
