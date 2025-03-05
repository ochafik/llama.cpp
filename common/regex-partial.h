#pragma once

#include <optional>
#include <regex>
#include <string>

struct common_regex_match {
    size_t pos;
    bool is_partial;
    bool operator==(const common_regex_match & other) const {
        return pos == other.pos && is_partial == other.is_partial;
    }
    bool operator!=(const common_regex_match & other) const {
        return !(*this == other);
    }
};

class common_regex {
    std::string pattern;
    std::regex rx;
    std::regex rx_reversed_partial;
    bool at_start_;

  public:
    common_regex(const std::string & pattern, bool at_start = false);

    std::optional<common_regex_match> search(const std::string & input) const;

    const std::string & str() const { return pattern; }
    bool at_start() const { return at_start_; }
};

// For testing only (pretty print of failures).
std::string regex_to_reversed_partial_regex(const std::string &pattern);
