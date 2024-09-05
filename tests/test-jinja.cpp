#include "jinja.hpp"

#include <fstream>
#include <iostream>
#include <string>
#include <json.hpp>


void test_render(const std::string & template_str, const json & bindings, const std::string & expected, const json & expected_context = json()) {
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
void test_error(const std::string & template_str, const json & bindings, const std::string & expected) {
    std::cout << "Testing: " << template_str << std::endl;
    try {
        auto root = JinjaParser::parse(template_str);
        auto context = Value::context(Value::make(bindings));
        // auto copy = context.is_null() ? Value::object() : std::make_shared<Value>(context);
        auto actual = root->render(*context);
        throw std::runtime_error("Expected error: "  + expected + ", but got successful result instead: "  + actual);
    } catch (const std::runtime_error & e) {
        auto actual = e.what();
        if (expected != actual) {
            std::cerr << "Expected: " << expected << std::endl;
            std::cerr << "Actual: " << actual << std::endl;
            throw std::runtime_error("Test failed");
        }
    }
    std::cout << "Test passed: " << template_str << std::endl;
}

inline std::string read_file(const std::string &path) {
  std::ifstream fs(path, std::ios_base::binary);
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
    test_error(
        "{{ raise_exception('hey') }}", json(),
        "hey");
    

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
        R"( {{ "a" -}} b {{- "c" }} )", json(),
        " abc ");

    test_error("{% else %}", json(), "Unexpected else at row 1, column 1: {% else %}");
    test_error("{% endif %}", json(), "Unexpected endif at row 1, column 1: {% endif %}");
    test_error("{% elif 1 %}", json(), "Unexpected elif at row 1, column 1: {% elif 1 %}");
    test_error("{% endblock %}", json(), "Unexpected endblock at row 1, column 1: {% endblock %}");
    test_error("{% endfor %}", json(), "Unexpected endfor at row 1, column 1: {% endfor %}");

    test_error("{% if 1 %}", json(), "Unterminated if at row 1, column 1: {% if 1 %}");
    test_error("{% block foo %}", json(), "Unterminated block at row 1, column 1: {% block foo %}");
    test_error("{% for x in 1 %}", json(), "Unterminated for at row 1, column 1: {% for x in 1 %}");
    test_error("{% if 1 %}{% else %}", json(), "Unterminated if at row 1, column 1: {% if 1 %}{% else %}");
    test_error("{% if 1 %}{% else %}{% elif 1 %}{% endif %}", json(), "Unterminated if at row 1, column 1: {% if 1 %}{% else %}{% elif 1 %}{% endif %}");


    test_render("{% if 1 %}{% elif 1 %}{% else %}{% endif %}", json(), "");
    
    test_render(
        "{% set x = [] %}{% set _ = x.append(1) %}{{ x | tojson(indent=2) }}", json(), 
        "[\n  1\n]");

    test_render(
        "{{ not [] }}", json(), 
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
    )", json(), "Hello Olivier");

    json context = {
        {"tools", {
            {{"function", {{"name", "ipython"}}}},
            {{"function", {{"name", "brave_search"}}}},
            {{"function", {{"name", "custom_tool"}}}},
            {{"function", {{"name", "wolfram_alpha"}}}}
        }},
        {"cutting_knowledge_date", "2023-04-01"},
        {"todays_date", "2024-09-03"}
    };

    std::string template_str = R"(
{%- set has_ipython = false -%}
{%- set predefined_tools = ['brave_search', 'wolfram_alpha'] -%}
{%- set displayed_tools = [] -%}
{%- set other_tools = [] -%}
{%- for tool in tools -%}
    {%- if tool.function.name == 'ipython' -%}
        {%- set has_ipython = true -%}
    {%- elif tool.function.name in predefined_tools -%}
        {%- set _ = displayed_tools.append(tool.function.name) -%}
    {%- else -%}
        {%- set _ = other_tools.append(tool) -%}
    {%- endif -%}
{%- endfor -%}
{%- if has_ipython -%}
Environment: ipython
{% endif %}
{%- if displayed_tools -%}
Tools: {{ displayed_tools | join }}
{{ displayed_tools }}
{# displayed_tools is sequence #}
{% endif %}
Cutting Knowledge Date: {{ cutting_knowledge_date }}
Today's Date: {{ todays_date }}
You are a helpful assistant with tool calling capabilities. When you receive a tool call response, use the output to format an answer to the original user question.
{%- if other_tools %}
You have access to the following functions: {{ other_tools | tojson }}
{%- endif -%}
    )";

// Tools: {{ displayed_tools | join(', ') }}
// You have access to the following functions: {{ other_tools | tojson(indent=2) }}

    // template_str = "{% for tool in tools %}";

    // auto root = JinjaParser::parse(template_str);
    // std::cout << root->render(std::make_shared<Value>(context)) << std::endl;
    test_render(template_str, context, R"()");

    return 0;
}