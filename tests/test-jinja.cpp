#include "jinja.hpp"

#include <fstream>
#include <iostream>
#include <string>
#include <json.hpp>

void announce_test(const std::string & name) {
    auto len = name.size();
    auto extract = jinja::strip(name);
    extract = json(name.substr(0, std::min<size_t>(len, 50)) + (len > 50 ? " [...]" : "")).dump();
    extract = extract.substr(1, extract.size() - 2);
    std::cout << "Testing: " << extract << std::endl << std::flush;
}

struct Timer {
    std::string name;
    std::chrono::time_point<std::chrono::high_resolution_clock> start;
    Timer(const std::string & name) : name(name), start(std::chrono::high_resolution_clock::now()) {}
    ~Timer() {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        std::cout << name << "  took " << duration_ms.count() << "ms" << std::endl << std::flush;
    }
};

void test_render(const std::string & template_str, const json & bindings, const std::string & expected, const json & expected_context = {}) {
    Timer timer("  ");
    announce_test(template_str);
    auto root = jinja::Parser::parse(template_str);
    auto context = jinja::Context::make(bindings);
    // std::cout << "Context: " << context.dump() << std::endl;
    std::string actual;
    try {
        actual = root->render(*context);
    } catch (const std::runtime_error & e) {
        actual = "ERROR: " + std::string(e.what());
        std::cerr << "AST: " << root->dump().dump(2) << std::endl << std::flush;
    }

    if (expected != actual) {
        std::cerr << "Expected: " << expected << std::endl;
        std::cerr << "Actual: " << actual << std::endl;
        std::cerr << std::flush;
        throw std::runtime_error("Test failed");
    }

    if (!expected_context.is_null()) {
        auto dump = context->dump();
        for (const auto & kv : expected_context.items()) {
            if (dump[kv.key()] != kv.value()) {
                std::cerr << "Expected context: " << expected_context.dump(2) << std::endl;
                std::cerr << "Actual context: " << dump.dump(2) << std::endl;
                std::cerr << std::flush;
                throw std::runtime_error("Test failed");
            }
        }
    }
    std::cout << "Test passed!" << std::endl << std::flush;
}
void test_error_contains(const std::string & template_str, const json & bindings, const std::string & expected) {
    Timer timer("  ");
    announce_test(template_str);
    try {
        auto root = jinja::Parser::parse(template_str);
        auto context = jinja::Context::make(bindings);
        // auto copy = context.is_null() ? Value::object() : std::make_shared<Value>(context);
        auto actual = root->render(*context);
        throw std::runtime_error("Expected error: " + expected + ", but got successful result instead: "  + actual);
    } catch (const std::runtime_error & e) {
        std::string actual(e.what());
        if (actual.find(expected) == std::string::npos) {
            std::cerr << "Expected: " << expected << std::endl;
            std::cerr << "Actual: " << actual << std::endl;
            std::cerr << std::flush;
            throw std::runtime_error("Test failed");
        }
    }
    std::cout << "  passed!" << std::endl << std::flush;
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
    test_render("{{ 'a' + [] | length + 'b' }}", {}, "a0b");
     test_render("{{ [1, 2, 3] | join(', ') + '...' }}", {}, "1, 2, 3...");
     test_render("{{ 'Tools: ' + [1, 2, 3] | reject('equalto', 2) | join(', ') + '...' }}", {}, "Tools: 1, 3...");
     test_render("{{ [1, 2, 3] | join(', ') }}", {}, "1, 2, 3");
     test_render("{% for i in range(3) %}{{i}},{% endfor %}", {}, "0,1,2,");
     test_render("{% set foo %}Hello {{ 'there' }}{% endset %}{{ 1 ~ foo ~ 2 }}", {}, "1Hello there2");
     test_render("{{ [1, False, null, True, 2, '3', 1, '3', False, null, True] | unique }}", {},
        "[1, False, null, True, 2, \"3\"]");
     test_render("{{ range(5) | length % 2 }}", {}, "1");
     test_render("{{ range(5) | length % 2 == 1 }},{{ [] | length > 0 }}", {}, "True,False");
     test_render(
        R"(
            {%- for x, y in [("a", "b"), ("c", "d")] -%}
                {{- x }},{{ y -}};
            {%- endfor -%}
        )", {}, "a,b;c,d;");
    test_render("{{ 1 is not string }}", {}, "True");
    test_render("{{ 'ab' * 3 }}", {}, "ababab");
    test_render("{{ [1, 2, 3][-1] }}", {}, "3");
    test_render(
        R"({%- set separator = joiner(' | ') -%}
           {%- for item in ["a", "b", "c"] %}{{ separator() }}{{ item }}{% endfor -%})",
        {}, "a | b | c");
    test_render(
        "{%- for i in range(0) -%}NAH{% else %}OK{% endfor %}",
        {},
        "OK");
    test_render(
        R"(
            {%- for i in range(5) -%}
                ({{ i }}, {{ loop.cycle('odd', 'even') }}),
            {%- endfor -%}
        )", {}, "(0, odd),(1, even),(2, odd),(3, even),(4, odd),");
    
    test_render(
        "{%- for i in range(5) if i % 2 == 0 -%}\n"
        "{{ i }}, first={{ loop.first }}, last={{ loop.last }}, index={{ loop.index }}, index0={{ loop.index0 }}, revindex={{ loop.revindex }}, revindex0={{ loop.revindex0 }}, prev={{ loop.previtem }}, next={{ loop.nextitem }},\n"
        "{% endfor -%}",
        {},
        "0, first=True, last=False, index=1, index0=0, revindex=3, revindex0=2, prev=, next=2,\n"
        "2, first=False, last=False, index=2, index0=1, revindex=2, revindex0=1, prev=0, next=4,\n"
        "4, first=False, last=True, index=3, index0=2, revindex=1, revindex0=0, prev=2, next=,\n");
    
    test_render(
        R"(
            {%- set res = [] -%}
            {%- for c in ["<", ">", "&", '"'] -%}
                {%- set _ = res.append(c | e) -%}
            {%- endfor -%}
            {{- res | join(", ") -}}
        )", {},
        R"(&lt;, &gt;, &amp;, &quot;)");
    test_render(
        R"(
            {%- set x = 1 -%}
            {%- set y = 2 -%}
            {%- macro foo(x, z, w=10) -%}
                x={{ x }}, y={{ y }}, z={{ z }}, w={{ w -}}
            {%- endmacro -%}
            {{- foo(100, 3) -}}
        )", {},
        R"(x=100, y=2, z=3, w=10)");
    test_render(
        R"(
            {% macro input(name, value='', type='text', size=20) -%}
                <input type="{{ type }}" name="{{ name }}" value="{{ value|e }}" size="{{ size }}">
            {%- endmacro -%}
    
            <p>{{ input('username') }}</p>
            <p>{{ input('password', type='password') }}</p>)",
        {}, R"(
            <p><input type="text" name="username" value="" size="20"></p>
            <p><input type="password" name="password" value="" size="20"></p>)");
    test_render(
        R"(
            {#- The values' default array should be created afresh at each call, unlike the equivalent Python function -#}
            {%- macro foo(values=[]) -%}
                {%- set _ = values.append(1) -%}
                {{- values -}}
            {%- endmacro -%}
            {{- foo() }} {{ foo() -}})",
        {}, R"([1] [1])");
    test_render(R"({{ None | items | tojson }}; {{ {1: 2} | items | tojson }})", {}, "[]; [[1, 2]]");
    test_render(R"({{ {1: 2, 3: 4, 5: 7} | dictsort | tojson }})", {}, "[[1, 2], [3, 4], [5, 7]]");
    test_render(R"({{ {1: 2}.items() }})", {}, "[[1, 2]]");
    test_render(R"({{ {1: 2}.get(1) }}; {{ {}.get(1) }}; {{ {}.get(1, 10) }})", {}, "2; ; 10");
    test_render(
        R"(
            {%- for x in [1, 1.2, "a", true, True, false, False, None, [], [1], [1, 2], {}, {"a": 1}, {1: "b"}] -%}
                {{- x | tojson -}},
            {%- endfor -%}
        )", {},
        R"(1,1.2,"a",True,True,False,False,null,[],[1],[1, 2],{},{"a": 1},{"1": "b"},)");
    test_render(
        R"(
            {%- set n = namespace(value=1, title='') -%}
            {{- n.value }} "{{ n.title }}",
            {%- set n.value = 2 -%}
            {%- set n.title = 'Hello' -%}
            {{- n.value }} "{{ n.title }}")", {}, R"(1 "",2 "Hello")");
    test_error_contains(
        "{{ (a.b.c) }}",
        {{"a", json({{"b", {{"c", 3}}}})}},
        "'a' is not defined");
    test_render(
        "{% set _ = a.b.append(c.d.e) %}{{ a.b }}",
        json::parse(R"({
            "a": {"b": [1, 2]},
            "c": {"d": {"e": 3}}
        })"),
        "[1, 2, 3]");

    test_render(R"(
        {%- for x, y in z -%}
            {{- x }},{{ y -}};
        {%- endfor -%}
    )", {{"z", json({json({1, 10}), json({2, 20})})}}, "1,10;2,20;");
    
    test_render("a\nb\n", {}, "a\nb");

    test_render(" a {{  'b' -}} c ", {}, " a bc ");
    test_render(" a {{- 'b'  }} c ", {}, " ab c ");
    test_render("a\n{{- 'b'  }}\nc", {}, "ab\nc");
    test_render("a\n{{  'b' -}}\nc", {}, "a\nbc");

    test_error_contains("{{ raise_exception('hey') }}", {}, "hey");
    
    test_render("{{ [] is iterable }}", {}, "True");
    test_render("{{ [] is not number }}", {}, "True");
    test_render("{% set x = [0, 1, 2, 3] %}{{ x[1:] }}{{ x[:2] }}{{ x[1:3] }}", {}, "[1, 2, 3][0, 1][1, 2]");
    test_render("{{ ' a  ' | trim }}", {}, "a");
    test_render("{{ range(3) }}{{ range(4, 7) }}{{ range(0, 10, step=2) }}", {}, "[0, 1, 2][4, 5, 6][0, 2, 4, 6, 8]");

    // List files
    for (const auto & entry : std::__fs::filesystem::directory_iterator("templates")) {
        if (entry.path().extension() != ".jinja") {
            continue;
        }
        std::string text_content = read_file(entry.path());
        //text_content = "{#- " + entry.path().string() + " -#}\n" + text_content;
        // std::cout << "# Parsing " << entry.path() << ":" << std::endl;
        // std::cout << text_content << std::endl;
        jinja::Parser::parse(text_content);
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
    
    auto test_file = [](const std::string & path, const json & bindings, const std::string & expected) {
        const auto tmpl = "{#- " + path + " -#}\n" + read_file(path);
        test_render(tmpl, bindings, expected);
    };

    test_file("templates/Meta-Llama-3.1-8B-Instruct.jinja",
        {
            {"messages", simple_messages},
            {"add_generation_prompt", true},
            {"tools", tools},
            {"builtin_tools", json::array({"wolfram_alpha", "brave_search"})},
            {"cutting_knowledge_date", "2023-04-01"},
            {"todays_date", "2024-09-03"},
            {"eos_token", "<|endoftext|>"},
            {"bos_token", "<|startoftext|>"},
        },
        "<|startoftext|><|start_header_id|>system<|end_header_id|>\n"
        "\n"
        "Environment: ipython\n"
        "Tools: wolfram_alpha, brave_search\n"
        "\n"
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

    test_file("templates/Hermes-2-Pro-Llama-3-8B.tool_use.jinja",
        {
            {"messages", simple_messages},
            {"add_generation_prompt", true},
            {"tools", tools},
            {"eos_token", "<|endoftext|>"},
            {"bos_token", "<|startoftext|>"},
        },
        R"(<|startoftext|><|im_start|>system
You are a function calling AI model. You are provided with function signatures within <tools></tools> XML tags. You may call one or more functions to assist with the user query. Don't make assumptions about what values to plug into functions. Here are the available tools: <tools> {"type": "function", "function": {"name": "ipython", "description": "ipython(code: str) - Runs code in an ipython interpreter and returns the result of the execution after 60 seconds.

    Args:
        code(str): None", "parameters": {"required": ["code"], "properties": {"code": {"type": "string"}}, "type": "object"}}
{"type": "function", "function": {"name": "brave_search", "description": "brave_search(code: Any) - Executes a web search with Brave.

    Args:
        code(Any): None", "parameters": {"required": ["query"], "properties": {"code": {"type": "query"}}, "type": "object"}}
{"type": "function", "function": {"name": "wolfram_alpha", "description": "wolfram_alpha(code: Any) - Executes a query with Wolfram Alpha.

    Args:
        code(Any): None", "parameters": {"required": ["query"], "properties": {"code": {"type": "query"}}, "type": "object"}}
{"type": "function", "function": {"name": "test", "description": "test(condition: bool) - Runs a test.

    Args:
        condition(bool): None", "parameters": {"required": ["condition"], "properties": {"condition": {"type": "boolean"}}, "type": "object"}} </tools>Use the following pydantic model json schema for each tool call you will make: {"properties": {"name": {"title": "Name", "type": "string"}, "arguments": {"title": "Arguments", "type": "object"}}, "required": ["name", "arguments"], "title": "FunctionCall", "type": "object"}}
For each function call return a json object with function name and arguments within <tool_call></tool_call> XML tags as follows:
<tool_call>
{"name": <function-name>, "arguments": <args-dict>}
</tool_call><|im_end|>
<|im_start|>system
System message<|im_end|>
<|im_start|>user
User message<|im_end|>
<|im_start|>assistant
Assistant message<|im_end|>
<|im_start|>assistant
)"
    );
        

    return 0;
}