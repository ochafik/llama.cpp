#include "jinja.hpp"

#include <iostream>
#include <string>
#include <json.hpp>

using json = nlohmann::json;

/*
    cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -t test-jinja -j && ./build/bin/test-jinja

    cmake -B buildDebug -DCMAKE_BUILD_TYPE=Debug && cmake --build buildDebug -t test-jinja -j && ./buildDebug/bin/test-jinja
*/
int main() {
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
{% set has_ipython = false %}
{% set predefined_tools = ['brave_search', 'wolfram_alpha'] %}
{% set displayed_tools = [] %}
{% set other_tools = [] %}
{% for tool in tools %}
    {% if tool.function.name == 'ipython' %}
        {% set has_ipython = true %}
    {% elif tool.function.name in predefined_tools %}
        {% set _ = displayed_tools.append(tool.function.name) %}
    {% else %}
        {% set _ = other_tools.append(tool) %}
    {% endif %}
{% endfor %}
{% if has_ipython %}
Environment: ipython
{% endif %}
{% if displayed_tools %}
Tools: {{ displayed_tools | join }}
{% endif %}
Cutting Knowledge Date: {{ cutting_knowledge_date }}
Today's Date: {{ todays_date }}
You are a helpful assistant with tool calling capabilities. When you receive a tool call response, use the output to format an answer to the original user question.
{% if other_tools %}
You have access to the following functions: {{ other_tools | tojson }}
{% endif %}
    )";

// Tools: {{ displayed_tools | join(', ') }}
// You have access to the following functions: {{ other_tools | tojson(indent=2) }}

    // template_str = "{% for tool in tools %}";

    auto root = JinjaParser::parse(template_str);
    std::cout << root->render(context) << std::endl;

    return 0;
}