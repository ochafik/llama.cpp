#include <json.hpp>
#include <variant>
#include <optional>

struct common_json {
    nlohmann::ordered_json json;
    std::string healing_marker;
    std::string json_healing_marker;
};

bool common_json_parse(
    std::string::const_iterator & it,
    const std::string::const_iterator & end,
    bool allow_healing,
    common_json & out);
