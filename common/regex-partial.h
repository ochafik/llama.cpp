#pragma once

#include <regex>
#include <string>

enum common_regex_match_type {
    COMMON_REGEX_MATCH_TYPE_NONE,
    COMMON_REGEX_MATCH_TYPE_PARTIAL,
    COMMON_REGEX_MATCH_TYPE_FULL,
};

struct common_regex_match_group {
    std::string str;
    size_t start_pos = std::string::npos;
    size_t end_pos = std::string::npos;
    bool operator==(const common_regex_match_group & other) const {
        return str == other.str && start_pos == other.start_pos && end_pos == other.end_pos;
    }
};

struct common_regex_match {
    common_regex_match_type type = COMMON_REGEX_MATCH_TYPE_NONE;
    std::vector<common_regex_match_group> groups;

    bool operator==(const common_regex_match & other) const {
        return type == other.type && groups == other.groups;
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

    common_regex_match search(const std::string & input) const;

    const std::string & str() const { return pattern; }
    bool at_start() const { return at_start_; }
};

// For testing only (pretty print of failures).
std::string regex_to_reversed_partial_regex(const std::string &pattern);
