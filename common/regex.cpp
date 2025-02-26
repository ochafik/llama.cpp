#include "regex.h"
#include "common.h"
#include <functional>

common_regex::common_regex(const std::string & pattern, bool at_start) :
    pattern(pattern),
    rx(pattern),
    rx_reversed_partial(regex_to_reversed_partial_regex(pattern)),
    at_start_(at_start) {}

std::optional<common_regex_match> common_regex::search(const std::string & input) const {
    std::smatch match;
    if (std::regex_search(input, match, rx)) {
        auto position = static_cast<size_t>(match.position());
        if (at_start_ && position != 0) {
            return std::nullopt;
        }
        return common_regex_match {
            /* .pos = */ position,
            /* .is_partial = */ false
        };
    }
    std::match_results<std::string::const_reverse_iterator> srmatch;
    if (std::regex_match(input.rbegin(), input.rend(), srmatch, rx_reversed_partial)) {
        auto group = srmatch[1].str();
        auto it = srmatch[1].second.base();
        auto position = static_cast<size_t>(std::distance(input.begin(), it));
        if (at_start_ && position != 0) {
            return std::nullopt;
        }
        return common_regex_match {
            /* .pos = */ position,
            /* .is_partial = */ true
        };
    }
    return std::nullopt;
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
  - /a.*b/ ->  ((?:b)?.*?a).* (in fact any repetition becomes a reluctant match!)
  - /a(bc)d/ -> ((?:(?:d)?(?:(?:c)?b))?a).*
  - /a(bc|de)/ -> ((?:(?:(?:e)?d)?|(?:(?:c)?b)?)?a).*
  - /ab{2,4}c/ -> abbb?b?c -> ((?:(?:(?:(?:(?:c)?b)?b)?b?)?b?)?a).*

  The regex will match a reversed string fully, and the end of the first (And only) capturing group will indicate the reversed start of the original partial pattern
  (i.e. just where the final .* starts in the inverted pattern; all other groups are turned into non-capturing groups, and reluctant quantifiers are ignored)
*/
std::string regex_to_reversed_partial_regex(const std::string &pattern) {
    auto it = pattern.begin();
    const auto end = pattern.end();

    std::function<std::string()> process = [&]() {
        std::vector<std::vector<std::string>> alternatives(1);
        std::vector<std::string> * sequence = &alternatives.back();

        while (it != end) {
            if (*it == '[') {
                auto start = it;
                while (it != end) {
                    if (*it == '\\' && (++it != end)) {
                        ++it;
                    } else if (*it == ']') {
                        break;
                    }
                }
                if (it == end) throw std::runtime_error("Unmatched '[' in pattern");
                ++it;
                sequence->push_back(std::string(start, it));
            } else if (*it == '*' || *it == '?') {
                if (sequence->empty()) throw std::runtime_error("Quantifier without preceding element");
                sequence->back() += *it;
                ++it;
                if (*it == '?') {
                    ++it;
                    // Convert initial reluctant quantifier to greedy to match as early as possible
                    if (sequence->size() > 1) {
                        sequence->back() += '?';
                    }
                } else {
                    // Convert greedy quantifiers to reluctant to not miss any matches
                    sequence->back() += '?';
                }
            } else if (*it == '{') {
                if (sequence->empty()) throw std::runtime_error("Repetition without preceding element");
                ++it;
                auto start = it;
                while (it != end && *it != '}') {
                    ++it;
                }
                if (it == end) throw std::runtime_error("Unmatched '{' in pattern");
                auto parts = string_split(std::string(start, it), ",");
                ++it;
                if (parts.size() > 2) throw std::runtime_error("Invalid repetition range in pattern");

                auto parseOptInt = [&](const std::string & s, const std::optional<int> & def = std::nullopt) -> std::optional<int> {
                    if (s.empty()) return def;
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
                if (*it != ')') throw std::runtime_error("Unmatched '(' in pattern");
                ++it;
                auto & part = sequence->emplace_back("(");
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
            } else {
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
    if (it != end) throw std::runtime_error("Unmatched '(' in pattern");

    return "(" + res + ").*";
}
