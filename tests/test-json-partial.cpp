#include "json-partial.h"
#include <iostream>

static void test_json_sax() {
  auto parse = [](const std::string & str) {
      std::cerr << "# Parsing: " << str << '\n';
      std::string::const_iterator it = str.begin();
      const auto end = str.end();
      return common_json::parse(it, end);
  };
  auto parse_all = [&](const std::string & str) {
      for (size_t i = 1; i < str.size() - 1; i++) {
          parse(str.substr(0, i));
      }
  };
  parse_all("{\"a\": \"b\"}");
  parse_all("{\"hey\": 1, \"ho\\\"ha\": [1]}");
}

int main() {
    test_json_sax();
    return 0;
}