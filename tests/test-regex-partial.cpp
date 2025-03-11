//  Tests common_regex (esp. its partial final matches support).

#include "regex-partial.h"

#include <sstream>
#include <iostream>

template <class T> static void assert_equals(const T & expected, const T & actual) {
    if (expected != actual) {
        std::cerr << "Expected: " << expected << std::endl;
        std::cerr << "  Actual: " << actual << std::endl;
        std::cerr << std::flush;
        throw std::runtime_error("Test failed");
    }
}

struct test_case {
    std::string pattern;
    bool at_start = false;
    struct input_output {
        std::string input;
        common_regex_match output;
        // bool operator==(const input_output & other) const {
        //     return input == other.input && output == other.output;
        // }
    };
    std::vector<input_output> inputs_outputs;
};

static void test_regex() {
    std::vector<test_case> test_cases {
        test_case {
            "a",
            /* .at_start = */ false,
            {
                {"a", {COMMON_REGEX_MATCH_TYPE_FULL, {{0, 1}}}},
                {"b", {COMMON_REGEX_MATCH_TYPE_NONE, {}}},
                {"ab", {COMMON_REGEX_MATCH_TYPE_FULL, {{0, 1}}}},
                {"ba", {COMMON_REGEX_MATCH_TYPE_FULL, {{1, 2}}}},
            }
        },
        test_case {
            "abcd",
            /* .at_start = */ false,
            {
                {"abcd", {COMMON_REGEX_MATCH_TYPE_FULL, {{0, 4}}}},
                {"abcde", {COMMON_REGEX_MATCH_TYPE_FULL, {{0, 4}}}},
                {"abc", {COMMON_REGEX_MATCH_TYPE_PARTIAL, {{0, 3}}}},
                {"ab", {COMMON_REGEX_MATCH_TYPE_PARTIAL, {{0, 2}}}},
                {"a", {COMMON_REGEX_MATCH_TYPE_PARTIAL, {{0, 1}}}},
                {"d", {}},
                {"bcd", {}},
                {"cde", {}},
                {"cd", {}},
                {"yeah ab", {COMMON_REGEX_MATCH_TYPE_PARTIAL, {{5, 7}}}},
                {"abbie", {}},
                {"", {}},
            }
        },
        test_case {
            ".*?ab",
            /* .at_start = */ false,
            {
                {"ab", {COMMON_REGEX_MATCH_TYPE_FULL, {{0, 2}}}},
                {"abc", {COMMON_REGEX_MATCH_TYPE_FULL, {{0, 2}}}},
                {"dab", {COMMON_REGEX_MATCH_TYPE_FULL, {{0, 3}}}},
                {"dabc", {COMMON_REGEX_MATCH_TYPE_FULL, {{0, 3}}}},
                {"da", {COMMON_REGEX_MATCH_TYPE_PARTIAL, {{0, 2}}}},
                {"d", {COMMON_REGEX_MATCH_TYPE_PARTIAL, {{0, 1}}}},
            }
        },
        test_case {
            "a.*?b",
            /* .at_start = */ false,
            {
                {"ab", {COMMON_REGEX_MATCH_TYPE_FULL, {{0, 2}}}},
                {"abc", {COMMON_REGEX_MATCH_TYPE_FULL, {{0, 2}}}},
                {"a b", {COMMON_REGEX_MATCH_TYPE_FULL, {{0, 3}}}},
                {"a", {COMMON_REGEX_MATCH_TYPE_PARTIAL, {{0, 1}}}},
                {"argh", {COMMON_REGEX_MATCH_TYPE_PARTIAL, {{0, 4}}}},
                {"d", {}},
                {"b", {}},
            }
        },
        test_case {
            "ab(?:cd){2,4}ef",
            /* .at_start = */ false,
            {
                // {"ab", {COMMON_REGEX_MATCH_TYPE_PARTIAL, 0, {}}},
                {"ab", {COMMON_REGEX_MATCH_TYPE_PARTIAL, {{0, 2}}}},
                {"abcd", {COMMON_REGEX_MATCH_TYPE_PARTIAL, {{0, 4}}}},
                {"abcde", {}},
                {"abcdef", {}},
                {"abcdcd", {COMMON_REGEX_MATCH_TYPE_PARTIAL, {{0, 6}}}},
                {"abcdcde", {COMMON_REGEX_MATCH_TYPE_PARTIAL, {{0, 7}}}},
                {"abcdcdef", {COMMON_REGEX_MATCH_TYPE_FULL, {{0, 8}}}},
                {"abcdcdcdcdef", {COMMON_REGEX_MATCH_TYPE_FULL, {{0, 12}}}},
                {"abcdcdcdcdcdef", {}},
                {"abcde", {}},
                {"yea", {COMMON_REGEX_MATCH_TYPE_PARTIAL, {{2, 3}}}},
            }
        },
        test_case {
            "a(?:rte| pure )fact",
            /* .at_start = */ false,
            {
                {"a", {COMMON_REGEX_MATCH_TYPE_PARTIAL, {{0, 1}}}},
                {"art", {COMMON_REGEX_MATCH_TYPE_PARTIAL, {{0, 3}}}},
                {"artefa", {COMMON_REGEX_MATCH_TYPE_PARTIAL, {{0, 6}}}},
                {"fact", {}},
                {"an arte", {COMMON_REGEX_MATCH_TYPE_PARTIAL, {{3, 7}}}},
                {"artefact", {COMMON_REGEX_MATCH_TYPE_FULL, {{0, 8}}}},
                {"an artefact", {COMMON_REGEX_MATCH_TYPE_FULL, {{3, 11}}}},
                {"a pure", {COMMON_REGEX_MATCH_TYPE_PARTIAL, {{0, 6}}}},
                {"a pure fact", {COMMON_REGEX_MATCH_TYPE_FULL, {{0, 11}}}},
                {"it's a pure fact", {COMMON_REGEX_MATCH_TYPE_FULL, {{5, 16}}}},
                {"" , {}},
                {"pure", {}},
                {"pure fact", {}},
            }
        },
        test_case {
            "abc",
            /* .at_start = */ true,
            {
                {" abcc", {}},
                {"ab", {COMMON_REGEX_MATCH_TYPE_PARTIAL, {{0, 2}}}},
                {"abc", {COMMON_REGEX_MATCH_TYPE_FULL, {{0, 3}}}},
                {" ab", {}},
            }
        },
    };

    for (const auto & test_case : test_cases) {
        common_regex cr(test_case.pattern, test_case.at_start);
        std::cout << "Testing pattern: /" << test_case.pattern << "/ (at_start = " << (test_case.at_start ? "true" : "false") << ")\n";
        // std::cout << "    partial rev: " << cr.reversed_partial_pattern.str() << '\n';
        for (const auto & input_output : test_case.inputs_outputs) {
            std::cout << "  Input: " << input_output.input << '\n';
            auto m = cr.search(input_output.input, 0);
            if (m != input_output.output) {
                auto match_to_str = [&](const std::optional<common_regex_match> & m) {
                    std::ostringstream ss;
                    if (m->type != COMMON_REGEX_MATCH_TYPE_NONE) {
                        ss << "pos = " << input_output.output.groups[0].begin << ", type = " << (m->type == COMMON_REGEX_MATCH_TYPE_PARTIAL ? "partial" : m->type == COMMON_REGEX_MATCH_TYPE_FULL ? "full" : "none") << ", groups.length = " << m->groups.size();
                    } else {
                        ss << "<no match>";
                    }
                    return ss.str();
                };
                std::cout << "    Expected: " << match_to_str(input_output.output) << '\n';
                std::cout << "         Got: " << match_to_str(m) << '\n';
                std::cout << " Inverted pattern: /" << regex_to_reversed_partial_regex(test_case.pattern) << "/\n";

                throw std::runtime_error("Test failed");
            }
        }
    }
}

static void test_regex_to_reversed_partial_regex() {
    assert_equals<std::string>(
        "(a+).*",
        regex_to_reversed_partial_regex("a+"));

    assert_equals<std::string>(
        "(a*?).*",
        regex_to_reversed_partial_regex("a*"));

    assert_equals<std::string>(
        "(a?).*",
        regex_to_reversed_partial_regex("a?"));
            
    assert_equals<std::string>(
        "([a-z]).*",
        regex_to_reversed_partial_regex("[a-z]"));
        
    assert_equals<std::string>(
        "((?:\\w+)?[a-z]).*",
        regex_to_reversed_partial_regex("[a-z]\\w+"));
    
    assert_equals<std::string>(
        "((?:a|b)).*",
        regex_to_reversed_partial_regex("(?:a|b)"));
    assert_equals<std::string>(
        "((?:(?:(?:d)?c)?b)?a).*",
        regex_to_reversed_partial_regex("abcd"));
    assert_equals<std::string>(
        "((?:b)?a*?).*", // TODO: ((?:b)?a*+).* ??
        regex_to_reversed_partial_regex("a*b"));
    assert_equals<std::string>(
        "((?:(?:b)?a)?.*).*",
        regex_to_reversed_partial_regex(".*?ab"));
    assert_equals<std::string>(
        "((?:(?:b)?.*?)?a).*",
        regex_to_reversed_partial_regex("a.*?b"));
    assert_equals<std::string>(
        "((?:(?:d)?(?:(?:c)?b))?a).*",
        regex_to_reversed_partial_regex("a(bc)d"));
    assert_equals<std::string>(
        "((?:(?:(?:c)?b|(?:e)?d))?a).*",
        regex_to_reversed_partial_regex("a(bc|de)"));
    assert_equals<std::string>(
        "((?:(?:(?:(?:(?:c)?b?)?b?)?b)?b)?a).*",
        regex_to_reversed_partial_regex("ab{2,4}c"));
}

int main() {
    try {
        test_regex_to_reversed_partial_regex();
        test_regex();
    } catch (const std::exception & e) {
        std::cerr << "Test failed: " << e.what() << '\n';
        return 1;
    }
    std::cout << "All tests passed.\n";
}
