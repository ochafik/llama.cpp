#include "jinja.hpp"

#include <fstream>
#include <iostream>
#include <string>
#include <json.hpp>

// using json = nlohmann::json;

void test_render(const std::string & template_str, const json & bindings, const std::string & expected, const json & expected_context = {}) {
    std::cout << "Testing: " << template_str << std::endl;
    auto root = JinjaParser::parse(template_str);
    auto context = Value::context(Value::make(bindings));
    auto actual = root->render(*context);

    if (expected != actual) {
        std::cerr << "Expected: " << expected << std::endl;
        std::cerr << "Actual: " << actual << std::endl;
        throw std::runtime_error("Test failed");
    }

    if (!expected_context.is_null()) {
        auto dump = context->get<json>();
        for (const auto & kv : expected_context.items()) {
            if (dump[kv.key()] != kv.value()) {
                std::cerr << "Expected context: " << expected_context.dump(2) << std::endl;
                std::cerr << "Actual context: " << dump.dump(2) << std::endl;
                throw std::runtime_error("Test failed");
            }
        }
    }
    std::cout << "Test passed: " << template_str << std::endl;
}
void test_error_contains(const std::string & template_str, const json & bindings, const std::string & expected) {
    std::cout << "Testing: " << template_str << std::endl;
    try {
        auto root = JinjaParser::parse(template_str);
        auto context = Value::context(Value::make(bindings));
        // auto copy = context.is_null() ? Value::object() : std::make_shared<Value>(context);
        auto actual = root->render(*context);
        throw std::runtime_error("Expected error: " + expected + ", but got successful result instead: "  + actual);
    } catch (const std::runtime_error & e) {
        std::string actual(e.what());
        if (actual.find(expected) == std::string::npos) {
            std::cerr << "Expected: " << expected << std::endl;
            std::cerr << "Actual: " << actual << std::endl;
            throw std::runtime_error("Test failed");
        }
    }
    std::cout << "Test passed: " << template_str << std::endl;
}

inline std::string read_file(const std::string &path) {
  std::ifstream fs(path, std::ios_base::binary);
  if (!fs.is_open()) {
    throw std::runtime_error("Failed to open file: " + path);
  }
  fs.seekg(0, std::ios_base::end);
  auto size = fs.tellg();
  fs.seekg(0);
  std::string out;
  out.resize(static_cast<size_t>(size));
  fs.read(&out[0], static_cast<std::streamsize>(size));
  return out;
}

/*
    cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -t test-jinja -j && ./build/bin/test-jinja

    cmake -B buildDebug -DCMAKE_BUILD_TYPE=Debug && cmake --build buildDebug -t test-jinja -j && ./buildDebug/bin/test-jinja
*/
int main() {
    test_render("{{ (a.b.c) }}", {{"a", json({{"b", {{"c", 3}}}})}}, "3");
    test_render(
        "{% set _ = a.b.append(c.d.e) %}{{ a.b }}",
        json::parse(R"({
            "a": {"b": [1, 2]},
            "c": {"d": {"e": 3}}
        })"),
        "[1,2,3]");

    test_render(R"(
        {%- for x, y in z -%}
            {{- x }},{{ y -}};
        {%- endfor -%}
    )", {{"z", json({json({1, 10}), json({2, 20})})}}, "1,10;2,20;");
    
    test_render("a\nb\n", {}, "a\nb");
    test_render(
        R"(
            {%- set n = namespace(value=1, title='') -%}
            {{- n.value }} "{{ n.title }}",
            {%- set n.value = 2 -%}
            {%- set n.title = 'Hello' -%}
            {{- n.value }} "{{ n.title }}")", {}, R"(1 "",2 "Hello")");

    test_render(" a {{  'b' -}} c ", {}, " a bc ");
    test_render(" a {{- 'b'  }} c ", {}, " ab c ");
    test_render("a\n{{- 'b'  }}\nc", {}, "ab\nc");
    test_render("a\n{{  'b' -}}\nc", {}, "a\nbc");

    test_error_contains("{{ raise_exception('hey') }}", {}, "hey");
    
    test_render("{{ [] is iterable }}", {}, "True");
    test_render("{{ [] is not number }}", {}, "True");
    test_render("{% set x = [0, 1, 2, 3] %}{{ x[1:] }}{{ x[:2] }}{{ x[1:3] }}", {}, "[1,2,3][0,1][1,2]");
    test_render("{{ ' a  ' | trim }}", {}, "a");
    test_render("{{ range(3) }}{{ range(4, 7) }}{{ range(0, 10, step=2) }}", {}, "[0,1,2][4,5,6][0,2,4,6,8]");

    // List files
    for (const auto & entry : std::__fs::filesystem::directory_iterator("templates")) {
        if (entry.path().extension() != ".jinja") {
            continue;
        }
        std::string text_content = read_file(entry.path());
        std::cout << "# Parsing " << entry.path() << ":" << std::endl;
        std::cout << text_content << std::endl;
        JinjaParser::parse(text_content);
    }

    test_render(
        R"( {{ "a" -}} b {{- "c" }} )", {},
        " abc ");

    test_error_contains("{% else %}", {}, "Unexpected else");
    test_error_contains("{% endif %}", {}, "Unexpected endif");
    test_error_contains("{% elif 1 %}", {}, "Unexpected elif");
    test_error_contains("{% endblock %}", {}, "Unexpected endblock");
    test_error_contains("{% endfor %}", {}, "Unexpected endfor");

    test_error_contains("{% if 1 %}", {}, "Unterminated if");
    test_error_contains("{% block foo %}", {}, "Unterminated block");
    test_error_contains("{% for x in 1 %}", {}, "Unterminated for");
    test_error_contains("{% if 1 %}{% else %}", {}, "Unterminated if");
    test_error_contains("{% if 1 %}{% else %}{% elif 1 %}{% endif %}", {}, "Unterminated if");


    test_render("{% if 1 %}{% elif 1 %}{% else %}{% endif %}", {}, "");
    
    test_render(
        "{% set x = [] %}{% set _ = x.append(1) %}{{ x | tojson(indent=2) }}", {}, 
        "[\n  1\n]");

    test_render(
        "{{ not [] }}", {}, 
        "True");
    
    test_render("{{ tool.function.name == 'ipython' }}", 
        json({{"tool", json({
            {"function", {{"name", "ipython"}}}
        })}}),
        "True");

    test_render(R"(
        {%- set user = "Olivier" -%}
        {%- set greeting = "Hello " ~ user -%}
        {{- greeting -}}
    )", {}, "Hello Olivier");

    std::string phi_template = R"(
        {%- for message in messages -%}
            {%- if message['role'] == 'system' and message['content'] -%}
                {{-'<|system|>\n' + message['content'] + '<|end|>\n'-}}
            {%- elif message['role'] == 'user' -%}
                {{-'<|user|>\n' + message['content'] + '<|end|>\n'-}}
            {%- elif message['role'] == 'assistant' -%}
                {{-'<|assistant|>\n' + message['content'] + '<|end|>\n'-}}
            {%- endif -%}
        {%- endfor -%}
        {%- if add_generation_prompt -%}
            {{- '<|assistant|>\n' -}}
        {%- else -%}
            {{- eos_token -}}
        {%- endif -%}
    )";

    const auto simple_messages = json::array({
        {{"role", "system"}, {"content", "System message"}},
        {{"role", "user"}, {"content", "User message"}},
        {{"role", "assistant"}, {"content", "Assistant message"}}
    });

    test_render(phi_template, json::object({
        {"messages", simple_messages},
        {"add_generation_prompt", true},
        {"eos_token", "<|endoftext|>"},
    }), 
        "<|system|>\n"
        "System message<|end|>\n"
        "<|user|>\n"
        "User message<|end|>\n"
        "<|assistant|>\n"
        "Assistant message<|end|>\n"
        "<|assistant|>\n"
    );

    const auto tools = json::parse(R"([
      {
        "type": "function",
        "function": {
          "name": "ipython",
          "description": "Runs code in an ipython interpreter and returns the result of the execution after 60 seconds.",
          "parameters": {
            "type": "object",
            "properties": {"code": {"type": "string"}},
            "required": ["code"]
          }
        }
      },
      {
        "type": "function",
        "function": {
          "name": "brave_search",
          "description": "Executes a web search with Brave.",
          "parameters": {
            "type": "object",
            "properties": {"code": {"type": "query"}},
            "required": ["query"]
          }
        }
      },
      {
        "type": "function",
        "function": {
          "name": "wolfram_alpha",
          "description": "Executes a query with Wolfram Alpha.",
          "parameters": {
            "type": "object",
            "properties": {"code": {"type": "query"}},
            "required": ["query"]
          }
        }
      },
      {
        "type": "function",
        "function": {
          "name": "test",
          "description": "Runs a test.",
          "parameters": {
            "type": "object",
            "properties": {"condition": {"type": "boolean"}},
            "required": ["condition"]
          }
        }
      }
    ])");

//     {{- "Tools: " + builtin_tools | reject('equalto', 'code_interpreter') | join(", ") + "\n\n"}}
// {%- endif %}

    const auto llama3_1_template = read_file("templates/Meta-Llama-3.1-8B-Instruct.jinja");
    test_render(llama3_1_template, json::object({    
        {"messages", simple_messages},
        {"add_generation_prompt", true},
        {"tools", tools},
        {"builtin_tools", json::array({"wolfram_alpha", "brave_search"})},
        {"cutting_knowledge_date", "2023-04-01"},
        {"todays_date", "2024-09-03"},
        {"eos_token", "<|endoftext|>"}
    }),
        "<|start_header_id|>system<|end_header_id|>\n"
        "\n"
        "Environment: ipython\n"
        "Cutting Knowledge Date: December 2023\n"
        "Today Date: 26 Jul 2024\n"
        "\n"
        "System message<|eot_id|><|start_header_id|>user<|end_header_id|>\n"
        "\n"
        "User message<|eot_id|><|start_header_id|>assistant<|end_header_id|>\n"
        "\n"
        "Assistant message<|eot_id|><|start_header_id|>assistant<|end_header_id|>\n"
        "\n"
        "\n"
    );
        

    return 0;
}