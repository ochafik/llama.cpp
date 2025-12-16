#include "regex-partial.h"
#include "common.h"
#include <functional>
#include <optional>

// Split a pattern by top-level alternation (|) for separate partial matching.
// This handles nested groups correctly - only splits at the top level.
std::vector<std::string> split_top_level_alternations(const std::string & pattern) {
    std::vector<std::string> alternatives;
    std::string current;
    int depth = 0;  // Track nesting depth of groups () and []

    for (size_t i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];
        if (c == '\\' && i + 1 < pattern.size()) {
            // Escape sequence - add both characters
            current += c;
            current += pattern[++i];
        } else if (c == '[') {
            // Character class - skip until ]
            current += c;
            ++i;
            while (i < pattern.size() && pattern[i] != ']') {
                if (pattern[i] == '\\' && i + 1 < pattern.size()) {
                    current += pattern[i++];
                }
                current += pattern[i++];
            }
            if (i < pattern.size()) {
                current += pattern[i];  // Add closing ]
            }
        } else if (c == '(') {
            depth++;
            current += c;
        } else if (c == ')') {
            depth--;
            current += c;
        } else if (c == '|' && depth == 0) {
            // Top-level alternation
            alternatives.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        alternatives.push_back(current);
    }
    return alternatives;
}

common_regex::common_regex(const std::string & pattern) :
    pattern(pattern),
    rx(pattern),
    rx_reversed_partial(regex_to_reversed_partial_regex(pattern)) {
    // If pattern has multiple top-level alternatives, store each one's reversed partial
    // separately to handle cases where one alternative can match empty.
    auto alts = split_top_level_alternations(pattern);
    if (alts.size() > 1) {
        for (const auto & alt : alts) {
            rx_reversed_partial_alts.emplace_back(regex_to_reversed_partial_regex(alt));
        }
    }
}

common_regex_match common_regex::search(const std::string & input, size_t pos, bool as_match) const {
    std::smatch match;
    if (pos > input.size()) {
        throw std::runtime_error("Position out of bounds");
    }
    auto start = input.begin() + pos;
    auto found = as_match
        ? std::regex_match(start, input.end(), match, rx)
        : std::regex_search(start, input.end(), match, rx);
    if (found) {
        common_regex_match res;
        res.type = COMMON_REGEX_MATCH_TYPE_FULL;
        for (size_t i = 0; i < match.size(); ++i) {
            auto begin = pos + match.position(i);
            res.groups.emplace_back(begin, begin + match.length(i));
        }
        return res;
    }

    // Helper lambda to check for partial match with a reversed partial regex
    auto try_partial_match = [&](const std::regex & rx_partial) -> common_regex_match {
        std::match_results<std::string::const_reverse_iterator> srmatch;
        if (std::regex_match(input.rbegin(), input.rend() - pos, srmatch, rx_partial)) {
            auto group = srmatch[1].str();
            if (group.length() != 0) {
                auto it = srmatch[1].second.base();
                if ((!as_match) || it == input.begin()) {
                    common_regex_match res;
                    res.type = COMMON_REGEX_MATCH_TYPE_PARTIAL;
                    const size_t begin = std::distance(input.begin(), it);
                    const size_t end = input.size();
                    if (begin == std::string::npos || end == std::string::npos || begin > end) {
                        throw std::runtime_error("Invalid range");
                    }
                    res.groups.push_back({begin, end});
                    return res;
                }
            }
        }
        return {};
    };

    // First try the combined pattern
    auto result = try_partial_match(rx_reversed_partial);
    if (result.type != COMMON_REGEX_MATCH_TYPE_NONE) {
        return result;
    }

    // If combined pattern didn't find a non-empty partial match but we have
    // multiple top-level alternatives, try each one separately.
    // This handles cases where one alternative can match empty and "steals"
    // the match from other alternatives that would match non-empty.
    for (const auto & alt_rx : rx_reversed_partial_alts) {
        result = try_partial_match(alt_rx);
        if (result.type != COMMON_REGEX_MATCH_TYPE_NONE) {
            return result;
        }
    }

    return {};
}

/*
  Transforms a regex pattern to a partial match pattern that operates on a reversed input string to find partial final matches of the original pattern.

  Ideally we'd like to use boost::match_partial (https://beta.boost.org/doc/libs/1_59_0/libs/regex/doc/html/boost_regex/partial_matches.html)
  to see if a string ends with a partial regex match, but but it's not in std::regex yet.
  Instead, we'll the regex into a partial match regex operating as a full match on the reverse iterators of the input.

  - /abcd/ -> (dcba|cba|ba|a).* -> ((?:(?:(?:(?:d)?c)?b)?a).*
  - /a|b/ -> (a|b).*
  - /a*?/ -> error, could match ""
  - /a*b/ -> ((?:b)?a*+).* (final repetitions become eager)
  - /.*?ab/ -> ((?:b)?a).* (merge .*)
  - /a.*?b/ -> ((?:b)?.*?a).* (keep reluctant matches)
  - /a(bc)d/ -> ((?:(?:d)?(?:(?:c)?b))?a).*
  - /a(bc|de)/ -> ((?:(?:(?:e)?d)?|(?:(?:c)?b)?)?a).*
  - /ab{2,4}c/ -> abbb?b?c -> ((?:(?:(?:(?:(?:c)?b)?b)?b?)?b?)?a).*

  The regex will match a reversed string fully, and the end of the first (And only) capturing group will indicate the reversed start of the original partial pattern
  (i.e. just where the final .* starts in the inverted pattern; all other groups are turned into non-capturing groups, and reluctant quantifiers are ignored)
*/
std::string regex_to_reversed_partial_regex(const std::string & pattern) {
    auto it = pattern.begin();
    const auto end = pattern.end();

    std::function<std::string()> process = [&]() {
        std::vector<std::vector<std::string>> alternatives(1);
        std::vector<std::string> * sequence = &alternatives.back();

        while (it != end) {
            if (*it == '[') {
                auto start = it;
                ++it;
                while (it != end) {
                    if ((*it == '\\') && (++it != end)) {
                        ++it;
                    } else if ((it != end) && (*it == ']')) {
                        break;
                    } else {
                        ++it;
                    }
                }
                if (it == end) {
                    throw std::runtime_error("Unmatched '[' in pattern");
                }
                ++it;
                sequence->push_back(std::string(start, it));
            } else if (*it == '*' || *it == '?' || *it == '+') {
                if (sequence->empty()) {
                    throw std::runtime_error("Quantifier without preceding element");
                }
                sequence->back() += *it;
                auto is_star = *it == '*';
                ++it;
                if (is_star) {
                    if (*it == '?') {
                        ++it;
                    }
                }
            } else if (*it == '{') {
                if (sequence->empty()) {
                    throw std::runtime_error("Repetition without preceding element");
                }
                ++it;
                auto start = it;
                while (it != end && *it != '}') {
                    ++it;
                }
                if (it == end) {
                    throw std::runtime_error("Unmatched '{' in pattern");
                }
                auto parts = string_split(std::string(start, it), ",");
                ++it;
                if (parts.size() > 2) {
                    throw std::runtime_error("Invalid repetition range in pattern");
                }

                auto parseOptInt = [&](const std::string & s, const std::optional<int> & def = std::nullopt) -> std::optional<int> {
                    if (s.empty()) {
                        return def;
                    }
                    return std::stoi(s);
                };
                auto min = parseOptInt(parts[0], 0);
                auto max = parts.size() == 1 ? min : parseOptInt(parts[1]);
                if (min && max && *max < *min) {
                    throw std::runtime_error("Invalid repetition range in pattern");
                }
                // Brutal but... let's repeat at least min times, then ? for the delta between min & max (or * for unbounded)
                auto part = sequence->back();
                sequence->pop_back();
                for (int i = 0; i < *min; i++) {
                    sequence->push_back(part);
                }
                if (max) {
                    for (int i = *min; i < *max; i++) {
                        sequence->push_back(part + "?");
                    }
                } else {
                    sequence->push_back(part + "*");
                }
            } else if (*it == '(') {
                ++it;
                if (it != end && *it == '?' && (it + 1 != end) && *(it + 1) == ':') {
                    it += 2;
                }
                auto sub = process();
                if (*it != ')') {
                    throw std::runtime_error("Unmatched '(' in pattern");
                }
                ++it;
                auto & part = sequence->emplace_back("(?:");
                part += sub;
                part += ")";
            } else if (*it == ')') {
                break;
            } else if (*it == '|') {
                ++it;
                alternatives.emplace_back();
                sequence = &alternatives.back();
            } else if (*it == '\\' && (++it != end)) {
                auto str = std::string("\\") + *it;
                sequence->push_back(str);
                ++it;
            } else if (it != end) {
                sequence->push_back(std::string(1, *it));
                ++it;
            }
        }

        // /abcd/ -> (dcba|cba|ba|a).* -> ((?:(?:(?:d)?c)?b)?a).*
        // if n(=4) parts, opening n-1(=3) non-capturing groups after the 1 capturing group
        // We'll do the outermost capturing group and final .* in the enclosing function.
        std::vector<std::string> res_alts;
        for (const auto & parts : alternatives) {
            auto & res = res_alts.emplace_back();
            for (size_t i = 0; i < parts.size() - 1; i++) {
                res += "(?:";
            }
            for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
                res += *it;
                if (it != parts.rend() - 1) {
                    res += ")?";
                }
            }
        }
        return string_join(res_alts, "|");
    };
    auto res = process();
    if (it != end) {
        throw std::runtime_error("Unmatched '(' in pattern");
    }

    return "(" + res + ")[\\s\\S]*";
}
