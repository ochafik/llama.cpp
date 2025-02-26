//  Tests common_regex (esp. its partial final matches support).

#include "regex.h"

#include <sstream>
#include <iostream>

struct test_case {
    std::string pattern;
    bool at_start = false;
    std::vector<std::pair<std::string, std::optional<common_regex_match>>> inputs_outputs;
};

static void test_regex() {
    std::vector<test_case> test_cases {
        {
            "a",
            /* .at_start = */ false,
            {
                {"a", common_regex_match {0, false}},
                {"b", std::nullopt},
                {"ab", common_regex_match {0, false}},
                {"ba", common_regex_match {1, false}},
            }
        },
        {
            "abcd",
            /* .at_start = */ false,
            {
                {"abcd", common_regex_match {0, false}},
                {"abc", common_regex_match {0, true}},
                {"bcd", std::nullopt},
                {"ab", common_regex_match {0, true}},
                {"cd", std::nullopt},
                {"a", common_regex_match {0, true}},
                {"d", std::nullopt},
                {"yeah ab", common_regex_match {5, true}},
                {"abbie", std::nullopt},
                {"", std::nullopt},
            }
        },
        {
            ".*?ab",
            /* .at_start = */ false,
            {
                {"ab", common_regex_match {0, false}},
                {"abc", common_regex_match {0, false}},
                {"dab", common_regex_match {0, false}},
                {"da", common_regex_match {0, true}},
                {"d", common_regex_match {0, true}},
                {"dabc", common_regex_match {0, false}},
            }
        },
        {
            "a.*?b",
            /* .at_start = */ false,
            {
                {"ab", common_regex_match {0, false}},
                {"abc", common_regex_match {0, false}},
                {"dab", common_regex_match {1, false}},
                {"dabc", common_regex_match {1, false}},
            }
        },
        {
            "ab(cd){2,4}ef",
            /* .at_start = */ false,
            {
                {"ab", common_regex_match {0, true}},
                {"abc", common_regex_match {0, true}},
                {"abcd", common_regex_match {0, true}},
                {"abcdc", common_regex_match {0, true}},
                {"abcde", std::nullopt},
                {"abcdcd", common_regex_match {0, true}},
                {"abcdcde", common_regex_match {0, true}},
                {"abcdcdef", common_regex_match {0, false}},
                {"abcdcdcdcdef", common_regex_match {0, false}},
                {"abcdcdcdcdcdef", std::nullopt},
                {"yea", common_regex_match {2, true}},
            }
        },
        {
            "a(rte| pure )fact",
            /* .at_start = */ false,
            {
                {"a", common_regex_match {0, true}},
                {"art", common_regex_match {0, true}},
                {"artefa", common_regex_match {0, true}},
                {"fact", std::nullopt},
                {"an arte", common_regex_match {3, true}},
                {"artefact", common_regex_match {0, false}},
                {"an artefact", common_regex_match {3, false}},
                {"a pure", common_regex_match {0, true}},
                {"a pure fact", common_regex_match {0, false}},
                {"it's a pure fact", common_regex_match {5, false}},
                {"" , std::nullopt},
                {"pure", std::nullopt},
                {"pure fact", std::nullopt},
            }
        },
        {
            "abc",
            /* .at_start = */ true,
            {
                {" abcc", std::nullopt},
                {"ab", common_regex_match {0, true}},
                {"abc", common_regex_match {0, false}},
                {" ab", std::nullopt},
            }
        },
    };

    for (const auto & test_case : test_cases) {
        common_regex cr(test_case.pattern, test_case.at_start);
        std::cout << "Testing pattern: /" << test_case.pattern << "/ (at_start = " << (test_case.at_start ? "true" : "false") << ")\n";
        // std::cout << "    partial rev: " << cr.reversed_partial_pattern.str() << '\n';
        for (const auto & input_output : test_case.inputs_outputs) {
            std::cout << "  Input: " << input_output.first << '\n';
            auto m = cr.search(input_output.first);
            if (m != input_output.second) {
                auto match_to_str = [](const std::optional<common_regex_match> & m) {
                    std::ostringstream ss;
                    if (m) {
                        ss << "pos = " << m->pos << ", is_partial = " << m->is_partial;
                    } else {
                        ss << "<no match>";
                    }
                    return ss.str();
                };
                std::cout << "    Expected: " << match_to_str(input_output.second) << '\n';
                std::cout << "         Got: " << match_to_str(m) << '\n';
                std::cout << " Inverted pattern: /" << regex_to_reversed_partial_regex(test_case.pattern) << "/\n";

                throw std::runtime_error("Test failed");
            }
        }
    }
}

int main(int, char **) {
    test_regex();
    return 0;
}
