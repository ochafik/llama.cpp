/*
  Minimalistic Jinja templating engine for llama.cpp. C++11, no deps (single-header), decent language support but very few functions (easy to extend), just what’s needed for actual prompt templates.
  
  Models have increasingly complex templates (e.g. Llama 3.1, Hermes 2 Pro w/ tool_use), so we need a proper template engine to get the best out of them.

  Supports:
  - Statements `{{% … %}}`, variable sections `{{ … }}`, and comments `{# … #}` with pre/post space elision `{%- … -%}` / `{{- … -}}` / `{#- … -#}`
  - `set` w/ namespaces & destructuring
  - `if` / `elif` / `else` / `endif`
  - `for` (`recursive`) (`if`) / `else` / `endfor` w/ `loop.*` (including `loop.cycle`) and destructuring
  - `macro` / `endmacro`
  - Extensible filters collection: `count`, `dictsort`, `equalto`, `e` / `escape`, `items`, `join`, `joiner`, `namespace`, `raise_exception`, `range`, `reject`, `tojson`, `trim`
  - Full expression syntax

  Not supported:
  - Most filters & pipes
  - No difference between none and undefined
  - Tuples
  - `if` expressions w/o `else` (but `if` statements are fine)
  - `{% raw %}`
  - `{% include … %}`, `{% extends …%},
  
  Model templates verified to work:
  - Meta-Llama-3.1-8B-Instruct
  - Phi-3.5-mini-instruct
  - Hermes-2-Pro-Llama-3-8B (default & tool_use variants)
  - Qwen2-VL-7B-Instruct, Qwen2-7B-Instruct
  - Mixtral-8x7B-Instruct-v0.1

  TODO:
  - Simplify
    - Pass tokens to IfNode and such
    - Remove MacroContext
  - Functionary 3.2:
      https://huggingface.co/meetkai/functionary-small-v3.2
      https://huggingface.co/meetkai/functionary-medium-v3.2 
    - selectattr("type", "defined")
      {{ users|selectattr("is_active") }}
      {{ users|selectattr("email", "none") }}
      {{ data | selectattr('name', '==', 'Jinja') | list | last }}
    - Macro nested set scope = global?
      {%- macro get_param_type(param) -%}
        {%- set param_type = "any" -%}

    - map(attribute="type")
  - Add |dict_update({...})
  - Add {%- if tool.parameters.properties | length == 0 %}
  - Add `{% raw %}{{ broken }{% endraw %}` https://jbmoelker.github.io/jinja-compat-tests/tags/raw/
  - Add more functions https://jinja.palletsprojects.com/en/3.0.x/templates/#builtin-filters
    - https://jbmoelker.github.io/jinja-compat-tests/
*/
#include "minja.hpp"

#include <fstream>
#include <iostream>
#include <string>
#include <json.hpp>

static std::string read_file(const std::string &path) {
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

static std::vector<std::string> find_files(const std::string & folder, const std::string & ext) {
    std::vector<std::string> files;
    for (const auto & entry : std::__fs::filesystem::directory_iterator(folder)) {
        if (entry.path().extension() == ext)
            files.push_back(entry.path().string());
    }
    return files;
}

static std::string filename_without_extension(const std::string & path) {
    auto res = path;
    auto pos = res.find_last_of('/');
    if (pos != std::string::npos)
        res = res.substr(pos + 1);
    pos = res.find_last_of('.');
    if (pos != std::string::npos)
        res = res.substr(0, pos);
    return res;
}

static void assert_equals(const std::string & expected, const std::string & actual) {
    if (expected != actual) {
        std::cerr << "Expected: " << expected << std::endl;
        std::cerr << "Actual: " << actual << std::endl;
        std::cerr << std::flush;
        throw std::runtime_error("Test failed");
    }
}

static void announce_test(const std::string & name, const minja::Options & options) {
    auto len = name.size();
    auto extract = minja::strip(name);
    extract = json(name.substr(0, std::min<size_t>(len, 50)) + (len > 50 ? " [...]" : "")).dump();
    extract = extract.substr(1, extract.size() - 2);
    std::cout << "Testing: " << extract;
    static const minja::Options default_options {};
    if (options.lstrip_blocks != default_options.lstrip_blocks)
        std::cout << " lstrip_blocks=" << options.lstrip_blocks;
    if (options.trim_blocks != default_options.trim_blocks)
        std::cout << " trim_blocks=" << options.trim_blocks;
    std::cout << std::endl << std::flush;
}

static void test_render(const std::string & template_str, const json & bindings, const minja::Options & options, const std::string & expected, const json & expected_context = {}) {
    announce_test(template_str, options);
    auto root = minja::Parser::parse(template_str, options);
    auto context = minja::Context::make(bindings);
    std::string actual;
    try {
        actual = root->render(*context);
    } catch (const std::runtime_error & e) {
        actual = "ERROR: " + std::string(e.what());
    }

    assert_equals(expected, actual);

    if (!expected_context.is_null()) {
        // auto dump = context->dump();
        for (const auto & kv : expected_context.items()) {
            auto value = context->get(kv.key());
            if (value != kv.value()) {
                std::cerr << "Expected context value for " << kv.key() << ": " << kv.value() << std::endl;
                std::cerr << "Actual value: " << value.dump() << std::endl;
                std::cerr << std::flush;
                throw std::runtime_error("Test failed");
            }
        }
    }
    std::cout << "Test passed!" << std::endl << std::flush;
}

static void test_error_contains(const std::string & template_str, const json & bindings, const minja::Options & options, const std::string & expected) {
    announce_test(template_str, options);
    try {
        auto root = minja::Parser::parse(template_str, options);
        auto context = minja::Context::make(bindings);
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

static void test_template_features() {
    test_render(R"({{ {"a": "b"} | tojson }})", {}, {}, R"({"a": "b"})");
    test_render(R"({{ {"a": "b"} }})", {}, {}, R"({'a': 'b'})");

    std::string trim_tmpl = 
        "\n"
        "  {% if true %}Hello{% endif %}  \n"
        "...\n"
        "\n";
     test_render(
        trim_tmpl,
        {}, { .trim_blocks = true }, "\n  Hello...\n");
     test_render(
        trim_tmpl,
        {}, {}, "\n  Hello  \n...\n");
     test_render(
        trim_tmpl,
        {}, { .lstrip_blocks = true }, "\nHello  \n...\n");
     test_render(
        trim_tmpl,
        {}, { .trim_blocks = true, .lstrip_blocks = true }, "\nHello...\n");

    test_render(
        R"({%- set separator = joiner(' | ') -%}
           {%- for item in ["a", "b", "c"] %}{{ separator() }}{{ item }}{% endfor -%})",
        {}, {}, "a | b | c");
    test_render("a\nb\n", {}, {}, "a\nb");
    test_render("  {{- ' a\n'}}", {}, {.trim_blocks = true}, " a\n");

    test_render(
        R"(
            {%- for x in range(3) -%}
                {%- if loop.first -%}
                    but first, mojitos!
                {%- endif -%}
                {{ loop.index }}{{ "," if not loop.last -}}
            {%- endfor -%}
        )", {}, {}, "but first, mojitos!1,2,3");
    test_render("{{ 'a' + [] | length + 'b' }}", {}, {}, "a0b");
    test_render("{{ [1, 2, 3] | join(', ') + '...' }}", {}, {}, "1, 2, 3...");
    test_render("{{ 'Tools: ' + [1, 2, 3] | reject('equalto', 2) | join(', ') + '...' }}", {}, {}, "Tools: 1, 3...");
    test_render("{{ [1, 2, 3] | join(', ') }}", {}, {}, "1, 2, 3");
    test_render("{% for i in range(3) %}{{i}},{% endfor %}", {}, {}, "0,1,2,");
    test_render("{% set foo %}Hello {{ 'there' }}{% endset %}{{ 1 ~ foo ~ 2 }}", {}, {}, "1Hello there2");
    test_render("{{ [1, False, null, True, 2, '3', 1, '3', False, null, True] | unique }}", {}, {},
        "[1, False, null, True, 2, '3']");
    test_render("{{ range(5) | length % 2 }}", {}, {}, "1");
    test_render("{{ range(5) | length % 2 == 1 }},{{ [] | length > 0 }}", {}, {}, "True,False");
    test_render(
        "{{ messages[0]['role'] != 'system' }}",
        {{"messages", json::array({json({{"role", "system"}})})}},
        {},
        "False");
    test_render(
        R"(
            {%- for x, y in [("a", "b"), ("c", "d")] -%}
                {{- x }},{{ y -}};
            {%- endfor -%}
        )", {}, {}, "a,b;c,d;");
    test_render("{{ 1 is not string }}", {}, {}, "True");
    test_render("{{ 'ab' * 3 }}", {}, {}, "ababab");
    test_render("{{ [1, 2, 3][-1] }}", {}, {}, "3");
    test_render(
        "{%- for i in range(0) -%}NAH{% else %}OK{% endfor %}",
        {}, {},
        "OK");
    test_render(
        R"(
            {%- for i in range(5) -%}
                ({{ i }}, {{ loop.cycle('odd', 'even') }}),
            {%- endfor -%}
        )", {}, {}, "(0, odd),(1, even),(2, odd),(3, even),(4, odd),");
    
    test_render(
        "{%- for i in range(5) if i % 2 == 0 -%}\n"
        "{{ i }}, first={{ loop.first }}, last={{ loop.last }}, index={{ loop.index }}, index0={{ loop.index0 }}, revindex={{ loop.revindex }}, revindex0={{ loop.revindex0 }}, prev={{ loop.previtem }}, next={{ loop.nextitem }},\n"
        "{% endfor -%}",
        {}, {},
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
        )", {}, {},
        R"(&lt;, &gt;, &amp;, &quot;)");
    test_render(
        R"(
            {%- set x = 1 -%}
            {%- set y = 2 -%}
            {%- macro foo(x, z, w=10) -%}
                x={{ x }}, y={{ y }}, z={{ z }}, w={{ w -}}
            {%- endmacro -%}
            {{- foo(100, 3) -}}
        )", {}, {},
        R"(x=100, y=2, z=3, w=10)");
    test_render(
        R"(
            {% macro input(name, value='', type='text', size=20) -%}
                <input type="{{ type }}" name="{{ name }}" value="{{ value|e }}" size="{{ size }}">
            {%- endmacro -%}
    
            <p>{{ input('username') }}</p>
            <p>{{ input('password', type='password') }}</p>)",
        {}, {}, R"(
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
        {}, {}, R"([1] [1])");
    test_render(R"({{ None | items | tojson }}; {{ {1: 2} | items | tojson }})", {}, {}, "[]; [[1, 2]]");
    test_render(R"({{ {1: 2, 3: 4, 5: 7} | dictsort | tojson }})", {}, {}, "[[1, 2], [3, 4], [5, 7]]");
    test_render(R"({{ {1: 2}.items() }})", {}, {}, "[[1, 2]]");
    test_render(R"({{ {1: 2}.get(1) }}; {{ {}.get(1) }}; {{ {}.get(1, 10) }})", {}, {}, "2; ; 10");
    test_render(
        R"(
            {%- for x in [1, 1.2, "a", true, True, false, False, None, [], [1], [1, 2], {}, {"a": 1}, {1: "b"}] -%}
                {{- x | tojson -}},
            {%- endfor -%}
        )", {}, {},
        R"(1,1.2,"a",True,True,False,False,null,[],[1],[1, 2],{},{"a": 1},{"1": "b"},)");
    test_render(
        R"(
            {%- set n = namespace(value=1, title='') -%}
            {{- n.value }} "{{ n.title }}",
            {%- set n.value = 2 -%}
            {%- set n.title = 'Hello' -%}
            {{- n.value }} "{{ n.title }}")", {}, {}, R"(1 "",2 "Hello")");
    test_error_contains(
        "{{ (a.b.c) }}",
        {{"a", json({{"b", {{"c", 3}}}})}},
        {},
        "'a' is not defined");
    test_render(
        "{% set _ = a.b.append(c.d.e) %}{{ a.b }}",
        json::parse(R"({
            "a": {"b": [1, 2]},
            "c": {"d": {"e": 3}}
        })"),
        {},
        "[1, 2, 3]");

    test_render(R"(
        {%- for x, y in z -%}
            {{- x }},{{ y -}};
        {%- endfor -%}
    )", {{"z", json({json({1, 10}), json({2, 20})})}}, {}, "1,10;2,20;");
    
    test_render(" a {{  'b' -}} c ", {}, {}, " a bc ");
    test_render(" a {{- 'b'  }} c ", {}, {}, " ab c ");
    test_render("a\n{{- 'b'  }}\nc", {}, {}, "ab\nc");
    test_render("a\n{{  'b' -}}\nc", {}, {}, "a\nbc");

    test_error_contains("{{ raise_exception('hey') }}", {}, {}, "hey");
    
    test_render("{{ [] is iterable }}", {}, {}, "True");
    test_render("{{ [] is not number }}", {}, {}, "True");
    test_render("{% set x = [0, 1, 2, 3] %}{{ x[1:] }}{{ x[:2] }}{{ x[1:3] }}", {}, {}, "[1, 2, 3][0, 1][1, 2]");
    test_render("{{ ' a  ' | trim }}", {}, {}, "a");
    test_render("{{ range(3) }}{{ range(4, 7) }}{{ range(0, 10, step=2) }}", {}, {}, "[0, 1, 2][4, 5, 6][0, 2, 4, 6, 8]");

    test_render(
        R"( {{ "a" -}} b {{- "c" }} )", {}, {},
        " abc ");

    test_error_contains("{% else %}", {}, {}, "Unexpected else");
    test_error_contains("{% endif %}", {}, {}, "Unexpected endif");
    test_error_contains("{% elif 1 %}", {}, {}, "Unexpected elif");
    test_error_contains("{% endfor %}", {}, {}, "Unexpected endfor");

    test_error_contains("{% if 1 %}", {}, {}, "Unterminated if");
    test_error_contains("{% for x in 1 %}", {}, {}, "Unterminated for");
    test_error_contains("{% if 1 %}{% else %}", {}, {}, "Unterminated if");
    test_error_contains("{% if 1 %}{% else %}{% elif 1 %}{% endif %}", {}, {}, "Unterminated if");

    test_render("{% if 1 %}{% elif 1 %}{% else %}{% endif %}", {}, {}, "");
    
    test_render(
        "{% set x = [] %}{% set _ = x.append(1) %}{{ x | tojson(indent=2) }}", {}, {}, 
        "[\n  1\n]");

    test_render(
        "{{ not [] }}", {}, {}, 
        "True");
    
    test_render("{{ tool.function.name == 'ipython' }}", 
        json({{"tool", json({
            {"function", {{"name", "ipython"}}}
        })}}),
        {},
        "True");

    test_render(R"(
        {%- set user = "Olivier" -%}
        {%- set greeting = "Hello " ~ user -%}
        {{- greeting -}}
    )", {}, {}, "Hello Olivier");
}

static void test_chat_templates_with_common_contexts_against_goldens() {
    auto jinja_template_files = find_files("tests/chat/templates", ".jinja");
    auto context_files = find_files("tests/chat/contexts", ".json");
    
    auto get_golden_file = [&](const std::string & tmpl_file, const std::string & ctx_file) {
        auto tmpl_name = filename_without_extension(tmpl_file);
        auto ctx_name = filename_without_extension(ctx_file);
        auto golden_name = tmpl_name + "-" + ctx_name;
        return "tests/chat/goldens/" + golden_name + ".txt";
    };
    auto fail_with_golden_instructions = [&]() {
        throw std::runtime_error("To fetch templates and generate golden files, run `python tests/update_jinja_goldens.py`");
    };
    if (jinja_template_files.empty()) {
        std::cerr << "No Jinja templates found in tests/chat/templates" << std::endl;
        fail_with_golden_instructions();
    }
    const auto options = minja::Options {.trim_blocks = true, .lstrip_blocks = true};
    for (const auto & tmpl_file : jinja_template_files) {
        std::cout << "# Testing template: " << tmpl_file << std::endl << std::flush;
        auto tmpl_str = read_file(tmpl_file);
        auto tmpl = minja::Parser::parse(tmpl_str, options);

        auto found_goldens = false;

        for (const auto & ctx_file : context_files) {
            auto ctx = json::parse(read_file(ctx_file));

            auto golden_file = get_golden_file(tmpl_file, ctx_file);
            if (!std::ifstream(golden_file).is_open()) {
                continue;
            }
            found_goldens = true;
            std::cout << "  - " << golden_file << std::endl << std::flush;

            std::string actual;
            try {
                actual = tmpl->render(*minja::Context::make(ctx));
            } catch (const std::runtime_error & e) {
                actual = "ERROR: " + std::string(e.what());
            }
            auto expected = read_file(golden_file);
            assert_equals(expected, actual);
        }

        if (!found_goldens) {
            std::cerr << "No golden files found for " << tmpl_file << std::endl;
            fail_with_golden_instructions();
        }
    }
}

/*
    cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -t test-minja -j && ./build/bin/test-minja
*/
int main() {
    test_template_features();

    if (getenv("LLAMA_SKIP_TESTS_SLOW_ON_EMULATOR")) {
        fprintf(stderr, "\033[33mWARNING: Skipping slow tests on emulator.\n\033[0m");
    } else {
        test_chat_templates_with_common_contexts_against_goldens();
    }

    return 0;
}