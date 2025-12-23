Skip to content
Navigation Menu
ggml-org
llama.cpp

Type / to search
Code
Issues
357
Pull requests
623
Discussions
Actions
Projects
1
Wiki
Security
9
Insights
Comparing changes
Choose two branches to see what’s changed or to start a new pull request. If you need to, you can also  or learn more about diff comparisons.
 
...
 
 Can’t automatically merge. Don’t worry, you can still create the pull request.
Discuss and review the changes in this comparison with others. Learn about pull requests
 2 contributors
 Commits 72
 Files changed 57
 Showing  with 7,841 additions and 3,753 deletions.
 271 changes: 271 additions & 0 deletions271  
AGENTS.md
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,271 @@
# Agent Guide: PEG Parser Migration

This document helps agents continue the work on fixing chat parsers for the PEG migration branch.

## Context

We're migrating chat template parsing from a monolithic implementation to modular PEG-based parsers. Each of the 26 supported formats now has its own file in `common/chat-parsers/`.

**Branch**: `token-aware-grammars-impl`

**Key files**:
- `common/chat-parsers/*.cpp` - Individual parser implementations
- `common/chat-peg-parser.cpp` - AST-to-message mapper
- `common/peg-parser.h` - PEG parser primitives
- `tests/test-chat.cpp` - Comprehensive tests including needle streaming tests

## Current State

### Static Tests
All static tests in `test_template_output_parsers()` pass.

### Streaming Tests
The needle streaming tests (`test_systematic_needle_streaming()`) test incremental parsing across multiple scenarios:

| Scenario | Description |
|----------|-------------|
| `content-no-tools` | Basic content streaming |
| `content-with-reasoning` | Thinking block + content |
| `reasoning-only` | Just reasoning, no content |
| `thinking-disabled` | Content with thinking explicitly disabled |
| `tools-available-but-disabled` | Tools provided but choice=none |
| `tool-auto-single` | Single tool call with content |
| `tool-required-only` | Tool call only, no content |
| `parallel-tool-calls` | Multiple tool calls |
| `tool-with-reasoning` | Thinking + content + tool call |

**Current results**: ~12 pass, ~122 fail

## Common Failure Patterns

### 1. "Reasoning: Never saw NEEDLE1"
**Cause**: Parser not extracting reasoning content correctly.

**Fix**: Check that the parser:
1. Has reasoning block support when `inputs.enable_thinking` is true
2. Uses `Tag::REASONING` to tag the reasoning content
3. Properly handles the `<think>...</think>` or format-specific tags

Example pattern:
```cpp
if (inputs.enable_thinking && extract_reasoning) {
    auto reasoning_content = p.tag(Tag::REASONING, p.until("</think>")) + ("</think>" | p.end());
    reasoning = p.optional("<think>" + reasoning_content);
}
```

### 2. "Content: Never saw NEEDLE1"
**Cause**: Content not being extracted when tools are present.

**Fix**: Ensure parser captures content before/after tool calls:
```cpp
auto content_before = p.tag(Tag::CONTENT, p.until("<tool_open>"));
auto content_after = p.tag(Tag::CONTENT, p.rest());
return reasoning << content_before << tool_calls << content_after;
```

### 3. "Tool call: Final tool call count mismatch"
**Cause**: Parser not producing expected number of tool calls.

**Fix**: Check:
1. Tool call tags (`TOOL_OPEN`, `TOOL_CLOSE`) are correct
2. `p.repeat()` min/max parameters match scenario
3. Grammar triggers are firing correctly

### 4. "Template returned empty parser definition"
**Cause**: Parser not defined for certain input combinations.

**Fix**: Ensure parser is built for all tool/thinking combinations, not just when `has_tools && inputs.tool_choice != NONE`.

### 5. "Test failed" (generic assertion)
**Cause**: Usually ID mismatch or whitespace differences.

**Fix**: Check test expectations match what parser produces. For formats with IDs (like Kimi K2), ensure `Tag::TOOL_ID` is used.

## How to Fix a Parser

### Step 1: Identify the failing template
```bash
./build/bin/test-chat 2>&1 | grep -A 20 "Testing needle streaming for TEMPLATE_NAME"
```

### Step 2: Understand what scenarios fail
Look for patterns like:
- All reasoning scenarios fail → reasoning not being parsed
- All tool scenarios fail → tool parsing broken
- Only `tool-with-reasoning` fails → interaction between reasoning and tools

### Step 3: Read the parser implementation
```bash
cat common/chat-parsers/TEMPLATE.cpp
```

Key things to check:
1. Is reasoning block handled when `inputs.enable_thinking` is true?
2. Is content captured before AND after tool calls?
3. Are all tags properly applied (REASONING, CONTENT, TOOL_NAME, TOOL_ARGS, TOOL_ID)?

### Step 4: Make the fix
Common fixes:

**Add reasoning support**:
```cpp
auto reasoning = p.eps();
if (inputs.enable_thinking && inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE) {
    auto reasoning_content = p.tag(Tag::REASONING, p.until("</think>")) + ("</think>" | p.end());
    reasoning = p.optional("<think>" + reasoning_content);
}
```

**Add content capture around tools**:
```cpp
auto content_before = p.tag(Tag::CONTENT, p.until("<tool_marker>"));
return reasoning << content_before << tool_calls;
```

**Add tool ID extraction** (for formats that include IDs):
```cpp
auto tool_open = p.token("<|tool_call_begin|>")
    + "functions." + p.literal_tag(Tag::TOOL_NAME, name) + ":"
    + p.tag(Tag::TOOL_ID, p.until("<|tool_call_argument_begin|>"))
    + "<|tool_call_argument_begin|>";
```

### Step 5: Rebuild and test
```bash
cmake --build build -j 8 --target test-chat
./build/bin/test-chat 2>&1 | grep -A 20 "Testing needle streaming for TEMPLATE_NAME"
```

### Step 6: Run full test suite
```bash
./build/bin/test-chat
```

Ensure "All tests passed!" appears at the end.

## Available Tags

From `common/chat-peg-parser.h`:

| Tag | Purpose |
|-----|---------|
| `REASONING` | Thinking/reasoning content |
| `CONTENT` | Assistant message content |
| `TOOL` | Tool call wrapper (optional) |
| `TOOL_OPEN` | Tool call opening marker |
| `TOOL_CLOSE` | Tool call closing marker |
| `TOOL_ID` | Tool call ID |
| `TOOL_NAME` | Function name |
| `TOOL_ARGS` | JSON arguments |
| `TOOL_ARG_NAME` | Individual arg name (constructed parsers) |
| `TOOL_ARG_STRING_VALUE` | String arg value (constructed parsers) |
| `TOOL_ARG_JSON_VALUE` | JSON arg value (constructed parsers) |

## Parser Helpers

From `common/peg-parser.h`:

| Helper | Description |
|--------|-------------|
| `p.literal("str")` | Match literal string |
| `p.token("str")` | Token-aware match (matches special token or falls back to text) |
| `p.literal_tag(Tag, "str")` | Match literal and tag it |
| `p.token_tag(Tag, "str")` | Token-aware match and tag it |
| `p.tag(Tag, expr)` | Tag an expression |
| `p.atomic_tag(Tag, expr)` | Tag atomically (all or nothing) |
| `p.until("str")` | Match until delimiter |
| `p.rest()` | Match rest of input |
| `p.optional(expr)` | Optional match |
| `p.repeat(expr, min, max)` | Repeat expression |
| `p.choice()` | Start building alternatives |
| `p.json()` | Match JSON value |
| `p.schema(expr, name, schema)` | Match with JSON schema validation |

## Priority Templates to Fix

Based on current pass rates (higher = easier to fix):

| Template | Passes | Fails | Notes |
|----------|--------|-------|-------|
| Apertus | 3 | 6 | Basic content works |
| Kimi K2 | 3 | 6 | Tool IDs fixed, reasoning issues |
| Command R7B | 1 | 8 | Some scenarios work |
| GLM 4.6 | 1 | 8 | Some scenarios work |
| Seed OSS | 1 | 8 | Some scenarios work |

## Test Infrastructure Notes

### Needle Test Structure
Each scenario injects "needles" (markers) into content/reasoning/args:
- `$N1C$`, `$N2C$` - Content needles
- `$N1R$`, `$N2R$` - Reasoning needles
- `$N1AK$_N`, `$N2AK$_N` - Arg key needles
- `$N1AV$_N`, `$N2AV$_N` - Arg value needles

Tests verify:
1. Needles appear in order (N1 before N2)
2. Content/reasoning/args stream incrementally
3. Final output matches expected message

### Updating Test Expectations

If a format includes tool IDs, tests must expect them:
```cpp
// Wrong - expects empty ID
message_assist_call

// Right - expects ID "0"
message_assist_call_idx

// Or inline:
simple_assist_msg("content", "reasoning", "tool_name", "{\"arg\": 1}", "0")
```
### Streaming Merge Fix
If tool call IDs aren't preserved in streaming tests, the merge logic in `test_parser_with_streaming` handles it:
```cpp
if (!diff.tool_call_delta.name.empty()) {
    merged.tool_calls.push_back({diff.tool_call_delta.name, "", diff.tool_call_delta.id});
}
```

## Useful Commands

```bash
# Build test binary
cmake --build build -j 8 --target test-chat

# Run all tests
./build/bin/test-chat

# Test specific template (grep output)
./build/bin/test-chat 2>&1 | grep -A 50 "Testing needle streaming for Hermes"

# Count passes/fails
./build/bin/test-chat 2>&1 | grep "✓" | wc -l
./build/bin/test-chat 2>&1 | grep "✗" | wc -l

# Categorize failures
./build/bin/test-chat 2>&1 | grep "✗" | sed 's/.*✗ //' | sort | uniq -c | sort -rn

# Find parser file
ls common/chat-parsers/*.cpp
```

## Recent Changes

1. **Kimi K2 TOOL_ID**: Added `Tag::TOOL_ID` extraction for `functions.name:id` format
2. **Streaming merge**: Fixed `test_parser_with_streaming` to preserve tool IDs
3. **Test constants**: Added `message_assist_call_idx`, `message_assist_call_content_idx`, etc.
4. **Removed key atomicity**: Argument keys can stream incrementally (only tool names must be atomic)

## Next Steps

1. Pick a template with few failures (Apertus, Kimi K2)
2. Identify failing scenarios
3. Fix reasoning/content/tool parsing as needed
4. Update test expectations if format includes IDs
5. Verify all tests pass
6. Commit and move to next template
  7 changes: 5 additions & 2 deletions7  
common/CMakeLists.txt
Original file line number	Diff line number	Diff line change
@@ -44,18 +44,21 @@ endif()

set(TARGET common)

# Glob chat parser files from the chat-parsers directory
file(GLOB CHAT_SYNTAX_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/chat-parsers/*.cpp")

add_library(${TARGET} STATIC
    arg.cpp
    arg.h
    base64.hpp
    chat-parser.cpp
    chat-parser.h
    chat-parser-xml-toolcall.h
    chat-parser-xml-toolcall.cpp
    chat-peg-parser.cpp
    chat-peg-parser.h
    chat-parsers-internal.h
    chat.cpp
    chat.h
    ${CHAT_SYNTAX_SOURCES}
    common.cpp
    common.h
    console.cpp
 879 changes: 0 additions & 879 deletions879  
common/chat-parser-xml-toolcall.cpp
Original file line number	Diff line number	Diff line change
@@ -1,879 +0,0 @@
#include "chat.h"
#include "chat-parser.h"
#include "common.h"
#include "json-partial.h"
#include "json-schema-to-grammar.h"
#include "log.h"
#include "regex-partial.h"

using json = nlohmann::ordered_json;

class xml_toolcall_syntax_exception : public std::runtime_error {
  public:
    xml_toolcall_syntax_exception(const std::string & message) : std::runtime_error(message) {}
};

template<typename T>
inline void sort_uniq(std::vector<T> &vec) {
    std::sort(vec.begin(), vec.end());
    vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
}

template<typename T>
inline bool all_space(const T &str) {
    return std::all_of(str.begin(), str.end(), [](unsigned char ch) { return std::isspace(ch); });
}

static size_t utf8_truncate_safe(const std::string_view s) {
    size_t len = s.size();
    if (len == 0) return 0;
    size_t i = len;
    for (size_t back = 0; back < 4 && i > 0; ++back) {
        --i;
        unsigned char c = s[i];
        if ((c & 0x80) == 0) {
            return len;
        } else if ((c & 0xC0) == 0xC0) {
            size_t expected_len = 0;
            if ((c & 0xE0) == 0xC0) expected_len = 2;
            else if ((c & 0xF0) == 0xE0) expected_len = 3;
            else if ((c & 0xF8) == 0xF0) expected_len = 4;
            else return i;
            if (len - i >= expected_len) {
                return len;
            } else {
                return i;
            }
        }
    }
    return len - std::min(len, size_t(3));
}

inline void utf8_truncate_safe_resize(std::string &s) {
    s.resize(utf8_truncate_safe(s));
}

inline std::string_view utf8_truncate_safe_view(const std::string_view s) {
    return s.substr(0, utf8_truncate_safe(s));
}

static std::optional<common_chat_msg_parser::find_regex_result> try_find_2_literal_splited_by_spaces(common_chat_msg_parser & builder, const std::string & literal1, const std::string & literal2) {
    if (literal1.size() == 0) return builder.try_find_literal(literal2);
    const auto saved_pos = builder.pos();
    while (auto res = builder.try_find_literal(literal1)) {
        builder.consume_spaces();
        const auto match_len = std::min(literal2.size(), builder.input().size() - builder.pos());
        if (builder.input().compare(builder.pos(), match_len, literal2, 0, match_len) == 0) {
            if (res->prelude.size() != res->groups[0].begin - saved_pos) {
                res->prelude = builder.str({saved_pos, res->groups[0].begin});
            }
            builder.move_to(builder.pos() + match_len);
            res->groups[0].end = builder.pos();
            GGML_ASSERT(res->groups[0].begin != res->groups[0].end);
            return res;
        }
        builder.move_to(res->groups[0].begin + 1);
    }
    builder.move_to(saved_pos);
    return std::nullopt;
}

/**
 * make a GBNF that accept any strings except those containing any of the forbidden strings.
 */
std::string make_gbnf_excluding(std::vector<std::string> forbids) {
    constexpr auto charclass_escape = [](unsigned char c) -> std::string {
        if (c == '\\' || c == ']' || c == '^' || c == '-') {
            std::string s = "\\";
            s.push_back((char)c);
            return s;
        }
        if (isprint(c)) {
            return std::string(1, (char)c);
        }
        char buf[16];
        snprintf(buf, 15, "\\x%02X", c);
        return std::string(buf);
    };
    constexpr auto build_expr = [charclass_escape](auto self, const std::vector<std::string>& forbids, int l, int r, int depth) -> std::string {
        std::vector<std::pair<unsigned char, std::pair<int,int>>> children;
        int i = l;
        while (i < r) {
            const std::string &s = forbids[i];
            if ((int)s.size() == depth) {
                ++i;
                continue;
            }
            unsigned char c = (unsigned char)s[depth];
            int j = i;
            while (j < r && (int)forbids[j].size() > depth &&
                   (unsigned char)forbids[j][depth] == c) {
                ++j;
            }
            children.push_back({c, {i, j}});
            i = j;
        }
        std::vector<std::string> alts;
        if (!children.empty()) {
            std::string cls;
            for (auto &ch : children) cls += charclass_escape(ch.first);
            alts.push_back(std::string("[^") + cls + "]");
        }
        for (auto &ch : children) {
            std::string childExpr = self(self, forbids, ch.second.first, ch.second.second, depth+1);
            if (!childExpr.empty()) {
                std::string quoted_ch = "\"";
                if (ch.first == '\\') quoted_ch += "\\\\";
                else if (ch.first == '"') quoted_ch += "\\\"";
                else if (isprint(ch.first)) quoted_ch.push_back(ch.first);
                else {
                    char buf[16];
                    snprintf(buf, 15, "\\x%02X", ch.first);
                    quoted_ch += buf;
                }
                quoted_ch += "\"";
                std::string branch = quoted_ch + std::string(" ") + childExpr;
                alts.push_back(branch);
            }
        }
        if (alts.empty()) return "";
        std::ostringstream oss;
        oss << "( ";
        for (size_t k = 0; k < alts.size(); ++k) {
            if (k) oss << " | ";
            oss << alts[k];
        }
        oss << " )";
        return oss.str();
    };
    if (forbids.empty()) return "( . )*";
    sort(forbids.begin(), forbids.end());
    std::string expr = build_expr(build_expr, forbids, 0, forbids.size(), 0);
    if (expr.empty()) {
        std::string cls;
        for (auto &s : forbids) if (!s.empty()) cls += charclass_escape((unsigned char)s[0]);
        expr = std::string("( [^") + cls + "] )";
    }
    if (forbids.size() == 1)
        return expr + "*";
    else
        return std::string("( ") + expr + " )*";
}

/**
 * Build grammar for xml-style tool call
 * form.scope_start and form.scope_end can be empty.
 * Requires data.format for model-specific hacks.
 */
void build_grammar_xml_tool_call(common_chat_params & data, const json & tools, const struct xml_tool_call_format & form) {
    GGML_ASSERT(!form.tool_start.empty());
    GGML_ASSERT(!form.tool_sep.empty());
    GGML_ASSERT(!form.key_start.empty());
    GGML_ASSERT(!form.val_end.empty());
    GGML_ASSERT(!form.tool_end.empty());

    std::string key_val_sep = form.key_val_sep;
    if (form.key_val_sep2) {
        key_val_sep += "\n";
        key_val_sep += *form.key_val_sep2;
    }
    GGML_ASSERT(!key_val_sep.empty());

    if (tools.is_array() && !tools.empty()) {
        data.grammar = build_grammar([&](const common_grammar_builder &builder) {
            auto string_arg_val = form.last_val_end ?
                    builder.add_rule("string-arg-val", make_gbnf_excluding({form.val_end, *form.last_val_end})) :
                    builder.add_rule("string-arg-val", make_gbnf_excluding({form.val_end}));

            std::vector<std::string> tool_rules;
            for (const auto & tool : tools) {
                if (!tool.contains("type") || tool.at("type") != "function" || !tool.contains("function")) {
                    LOG_WRN("Skipping tool without function: %s", tool.dump(2).c_str());
                    continue;
                }
                const auto & function = tool.at("function");
                if (!function.contains("name") || !function.at("name").is_string()) {
                    LOG_WRN("Skipping invalid function (invalid name): %s", function.dump(2).c_str());
                    continue;
                }
                if (!function.contains("parameters") || !function.at("parameters").is_object()) {
                    LOG_WRN("Skipping invalid function (invalid parameters): %s", function.dump(2).c_str());
                    continue;
                }
                std::string name = function.at("name");
                auto parameters = function.at("parameters");
                builder.resolve_refs(parameters);

                struct parameter_rule {
                    std::string symbol_name;
                    bool is_required;
                };
                std::vector<parameter_rule> arg_rules;
                if (!parameters.contains("properties") || !parameters.at("properties").is_object()) {
                    LOG_WRN("Skipping invalid function (invalid properties): %s", function.dump(2).c_str());
                    continue;
                } else {
                    std::vector<std::string> requiredParameters;
                    if (parameters.contains("required")) {
                        try { parameters.at("required").get_to(requiredParameters); }
                        catch (const std::runtime_error&) {
                            LOG_WRN("Invalid function required parameters, ignoring: %s", function.at("required").dump(2).c_str());
                        }
                    }
                    sort_uniq(requiredParameters);
                    for (const auto & [key, value] : parameters.at("properties").items()) {
                        std::string quoted_key = key;
                        bool required = std::binary_search(requiredParameters.begin(), requiredParameters.end(), key);
                        if (form.key_start.back() == '"' && key_val_sep[0] == '"') {
                            quoted_key = gbnf_format_literal(key);
                            quoted_key = quoted_key.substr(1, quoted_key.size() - 2);
                        }
                        arg_rules.push_back(parameter_rule {builder.add_rule("func-" + name + "-kv-" + key,
                            gbnf_format_literal(form.key_start) + " " +
                            gbnf_format_literal(quoted_key) + " " +
                            gbnf_format_literal(key_val_sep) + " " +
                            ((value.contains("type") && value["type"].is_string() && value["type"] == "string" && (!form.raw_argval || *form.raw_argval)) ?
                                    (form.raw_argval ?
                                            string_arg_val :
                                            "( " + string_arg_val + " | " + builder.add_schema(name + "-arg-" + key, value) + " )"
                                    ) :
                                    builder.add_schema(name + "-arg-" + key, value)
                            )
                        ), required});
                    }
                }

                auto next_arg_with_sep = builder.add_rule(name + "-last-arg-end", form.last_val_end ? gbnf_format_literal(*form.last_val_end) : gbnf_format_literal(form.val_end));
                decltype(next_arg_with_sep) next_arg = "\"\"";
                for (auto i = arg_rules.size() - 1; /* i >= 0 && */ i < arg_rules.size(); --i) {
                    std::string include_this_arg = arg_rules[i].symbol_name + " " + next_arg_with_sep;
                    next_arg = builder.add_rule(name + "-arg-after-" + std::to_string(i), arg_rules[i].is_required ?
                            include_this_arg : "( " + include_this_arg + " ) | " + next_arg
                    );
                    include_this_arg = gbnf_format_literal(form.val_end) + " " + include_this_arg;
                    next_arg_with_sep = builder.add_rule(name + "-arg-after-" + std::to_string(i) + "-with-sep", arg_rules[i].is_required ?
                            include_this_arg : "( " + include_this_arg + " ) | " + next_arg_with_sep
                    );
                }

                std::string quoted_name = name;
                if (form.tool_start.back() == '"' && form.tool_sep[0] == '"') {
                    quoted_name = gbnf_format_literal(name);
                    quoted_name = quoted_name.substr(1, quoted_name.size() - 2);
                }
                quoted_name = gbnf_format_literal(quoted_name);
                // Kimi-K2 uses functions.{{ tool_call['function']['name'] }}:{{ loop.index }} as function name
                if (data.format == COMMON_CHAT_FORMAT_KIMI_K2) {
                    quoted_name = "\"functions.\" " + quoted_name + " \":\" [0-9]+";
                }
                tool_rules.push_back(builder.add_rule(name + "-call",
                        gbnf_format_literal(form.tool_start) + " " +
                        quoted_name + " " +
                        gbnf_format_literal(form.tool_sep) + " " +
                        next_arg
                ));
            }

            auto tool_call_once = builder.add_rule("root-tool-call-once", string_join(tool_rules, " | "));
            auto tool_call_more = builder.add_rule("root-tool-call-more", gbnf_format_literal(form.tool_end) + " " + tool_call_once);
            auto call_end = builder.add_rule("root-call-end", form.last_tool_end ? gbnf_format_literal(*form.last_tool_end) : gbnf_format_literal(form.tool_end));
            auto tool_call_multiple_with_end = builder.add_rule("root-tool-call-multiple-with-end", tool_call_once + " " + tool_call_more + "* " + call_end);
            builder.add_rule("root",
                (form.scope_start.empty() ? "" : gbnf_format_literal(form.scope_start) + " ") +
                tool_call_multiple_with_end  + "?" +
                (form.scope_end.empty() ? "" : " " + gbnf_format_literal(form.scope_end))
            );
        });

        // grammar trigger for tool call
        data.grammar_triggers.push_back({ COMMON_GRAMMAR_TRIGGER_TYPE_WORD, form.scope_start + form.tool_start });
    }
}

/**
 * Parse XML-Style tool call for given xml_tool_call_format. Return false for invalid syntax and get the position untouched.
 * Throws xml_toolcall_syntax_exception if there is invalid syntax and cannot recover the original status for common_chat_msg_parser.
 * form.scope_start, form.tool_sep and form.scope_end can be empty.
 */
inline bool parse_xml_tool_calls(common_chat_msg_parser & builder, const struct xml_tool_call_format & form) {
    GGML_ASSERT(!form.tool_start.empty());
    GGML_ASSERT(!form.key_start.empty());
    GGML_ASSERT(!form.key_val_sep.empty());
    GGML_ASSERT(!form.val_end.empty());
    GGML_ASSERT(!form.tool_end.empty());

    // Helper to choose return false or throw error
    constexpr auto return_error = [](common_chat_msg_parser & builder, auto &start_pos, const bool &recovery) {
        LOG_DBG("Failed to parse XML-Style tool call at position: %s\n", gbnf_format_literal(builder.consume_rest().substr(0, 20)).c_str());
        if (recovery) {
            builder.move_to(start_pos);
            return false;
        } else throw xml_toolcall_syntax_exception("Tool call parsing failed with unrecoverable errors. Try using a grammar to constrain the model’s output.");
    };
    // Drop substring from needle to end from a JSON
    constexpr auto partial_json = [](std::string &json_str, std::string_view needle = "XML_TOOL_CALL_PARTIAL_FLAG") {
        auto pos = json_str.rfind(needle);
        if (pos == std::string::npos) {
            return false;
        }
        for (auto i = pos + needle.size(); i < json_str.size(); ++i) {
            unsigned char ch = static_cast<unsigned char>(json_str[i]);
            if (ch != '\'' && ch != '"' && ch != '}' && ch != ':' && !std::isspace(ch)) {
                return false;
            }
        }
        if (pos != 0 && json_str[pos - 1] == '"') {
            --pos;
        }
        json_str.resize(pos);
        return true;
    };
    // Helper to generate a partial argument JSON
    constexpr auto gen_partial_json = [partial_json](auto set_partial_arg, auto &arguments, auto &builder, auto &function_name) {
        auto rest = builder.consume_rest();
        utf8_truncate_safe_resize(rest);
        set_partial_arg(rest, "XML_TOOL_CALL_PARTIAL_FLAG");
        auto tool_str = arguments.dump();
        if (partial_json(tool_str)) {
            if (builder.add_tool_call(function_name, "", tool_str)) {
                return;
            }
        }
        LOG_DBG("Failed to parse partial XML-Style tool call, fallback to non-partial: %s\n", tool_str.c_str());
    };
    // Helper to find a close (because there may be form.last_val_end or form.last_tool_end)
    constexpr auto try_find_close = [](
            common_chat_msg_parser & builder,
            const std::string & end,
            const std::optional<std::string> & alt_end,
            const std::string & end_next,
            const std::optional<std::string> & alt_end_next
    ) {
        auto saved_pos = builder.pos();
        auto tc = builder.try_find_literal(end);
        auto val_end_size = end.size();
        if (alt_end) {
            auto pos_1 = builder.pos();
            builder.move_to(saved_pos);
            auto tc2 = try_find_2_literal_splited_by_spaces(builder, *alt_end, end_next);
            if (alt_end_next) {
                builder.move_to(saved_pos);
                auto tc3 = try_find_2_literal_splited_by_spaces(builder, *alt_end, *alt_end_next);
                if (tc3 && (!tc2 || tc2->prelude.size() > tc3->prelude.size())) {
                    tc2 = tc3;
                }
            }
            if (tc2 && (!tc || tc->prelude.size() > tc2->prelude.size())) {
                tc = tc2;
                tc->groups[0].end = std::min(builder.input().size(), tc->groups[0].begin + alt_end->size());
                builder.move_to(tc->groups[0].end);
                val_end_size = alt_end->size();
            } else {
                builder.move_to(pos_1);
            }
        }
        return std::make_pair(val_end_size, tc);
    };
    // Helper to find a val_end or last_val_end, returns matched pattern size
    const auto try_find_val_end = [try_find_close, &builder, &form]() {
        return try_find_close(builder, form.val_end, form.last_val_end, form.tool_end, form.last_tool_end);
    };
    // Helper to find a tool_end or last_tool_end, returns matched pattern size
    const auto try_find_tool_end = [try_find_close, &builder, &form]() {
        return try_find_close(builder, form.tool_end, form.last_tool_end, form.scope_end, std::nullopt);
    };

    bool recovery = true;
    const auto start_pos = builder.pos();
    if (!all_space(form.scope_start)) {
        if (auto tc = builder.try_find_literal(form.scope_start)) {
            if (all_space(tc->prelude)) {
                if (form.scope_start.size() != tc->groups[0].end - tc->groups[0].begin)
                    throw common_chat_msg_partial_exception("Partial literal: " + gbnf_format_literal(form.scope_start));
            } else {
                builder.move_to(start_pos);
                return false;
            }
        } else return false;
    }
    while (auto tc = builder.try_find_literal(form.tool_start)) {
        if (!all_space(tc->prelude)) {
            LOG_DBG("XML-Style tool call: Expected %s, but found %s, trying to match next pattern\n",
                    gbnf_format_literal(form.tool_start).c_str(),
                    gbnf_format_literal(tc->prelude).c_str()
            );
            builder.move_to(tc->groups[0].begin - tc->prelude.size());
            break;
        }

        // Find tool name
        auto func_name = builder.try_find_literal(all_space(form.tool_sep) ? form.key_start : form.tool_sep);
        if (!func_name) {
            auto [sz, tc] = try_find_tool_end();
            func_name = tc;
        }
        if (!func_name) {
            // Partial tool name not supported
            throw common_chat_msg_partial_exception("incomplete tool_call");
        }
        // If the model generate multiple tool call and the first tool call has no argument
        if (func_name->prelude.find(form.tool_end) != std::string::npos || (form.last_tool_end ? func_name->prelude.find(*form.last_tool_end) != std::string::npos : false)) {
            builder.move_to(func_name->groups[0].begin - func_name->prelude.size());
            auto [sz, tc] = try_find_tool_end();
            func_name = tc;
        }

        // Parse tool name
        builder.move_to(all_space(form.tool_sep) ? func_name->groups[0].begin : func_name->groups[0].end);
        std::string function_name = string_strip(func_name->prelude);
        // Kimi-K2 uses functions.{{ tool_call['function']['name'] }}:{{ loop.index }} as function name
        if (builder.syntax().format == COMMON_CHAT_FORMAT_KIMI_K2) {
            if (string_starts_with(function_name, "functions.")) {
                static const std::regex re(":\\d+$");
                if (std::regex_search(function_name, re)) {
                    function_name = function_name.substr(10, function_name.rfind(":") - 10);
                }
            }
        }

        // Argument JSON
        json arguments = json::object();

        // Helper to generate a partial argument JSON
        const auto gen_partial_args = [&](auto set_partial_arg) {
            gen_partial_json(set_partial_arg, arguments, builder, function_name);
        };

        // Parse all arg_key/arg_value pairs
        while (auto tc = builder.try_find_literal(form.key_start)) {
            if (!all_space(tc->prelude)) {
                LOG_DBG("XML-Style tool call: Expected %s, but found %s, trying to match next pattern\n",
                        gbnf_format_literal(form.key_start).c_str(),
                        gbnf_format_literal(tc->prelude).c_str()
                );
                builder.move_to(tc->groups[0].begin - tc->prelude.size());
                break;
            }
            if (tc->groups[0].end - tc->groups[0].begin != form.key_start.size()) {
                auto tool_call_arg = arguments.dump();
                if (tool_call_arg.size() != 0 && tool_call_arg[tool_call_arg.size() - 1] == '}') {
                    tool_call_arg.resize(tool_call_arg.size() - 1);
                }
                builder.add_tool_call(function_name, "", tool_call_arg);
                throw common_chat_msg_partial_exception("Partial literal: " + gbnf_format_literal(form.key_start));
            }

            // Parse arg_key
            auto key_res = builder.try_find_literal(form.key_val_sep);
            if (!key_res) {
                gen_partial_args([&](auto &rest, auto &needle) {arguments[rest + needle] = "";});
                throw common_chat_msg_partial_exception("Expected " + gbnf_format_literal(form.key_val_sep) + " after " + gbnf_format_literal(form.key_start));
            }
            if (key_res->groups[0].end - key_res->groups[0].begin != form.key_val_sep.size()) {
                gen_partial_args([&](auto &, auto &needle) {arguments[key_res->prelude + needle] = "";});
                throw common_chat_msg_partial_exception("Partial literal: " + gbnf_format_literal(form.key_val_sep));
            }
            auto &key = key_res->prelude;
            recovery = false;

            // Parse arg_value
            if (form.key_val_sep2) {
                if (auto tc = builder.try_find_literal(*form.key_val_sep2)) {
                    if (!all_space(tc->prelude)) {
                        LOG_DBG("Failed to parse XML-Style tool call: Unexcepted %s between %s and %s\n",
                                gbnf_format_literal(tc->prelude).c_str(),
                                gbnf_format_literal(form.key_val_sep).c_str(),
                                gbnf_format_literal(*form.key_val_sep2).c_str()
                        );
                        return return_error(builder, start_pos, false);
                    }
                    if (tc->groups[0].end - tc->groups[0].begin != form.key_val_sep2->size()) {
                        gen_partial_args([&](auto &, auto &needle) {arguments[key] = needle;});
                        throw common_chat_msg_partial_exception("Partial literal: " + gbnf_format_literal(*form.key_val_sep2));
                    }
                } else {
                    gen_partial_args([&](auto &, auto &needle) {arguments[key] = needle;});
                    throw common_chat_msg_partial_exception("Expected " + gbnf_format_literal(*form.key_val_sep2) + " after " + gbnf_format_literal(form.key_val_sep));
                }
            }
            auto val_start = builder.pos();

            // Test if arg_val is a partial JSON
            std::optional<common_json> value_json = std::nullopt;
            if (!form.raw_argval || !*form.raw_argval) {
                try { value_json = builder.try_consume_json(); }
                catch (const std::runtime_error&) { builder.move_to(val_start); }
                // TODO: Delete this when json_partial adds top-level support for null/true/false
                if (builder.pos() == val_start) {
                    const static std::regex number_regex(R"([0-9-][0-9]*(\.\d*)?([eE][+-]?\d*)?)");
                    builder.consume_spaces();
                    std::string_view sv = utf8_truncate_safe_view(builder.input());
                    sv.remove_prefix(builder.pos());
                    std::string rest = "a";
                    if (sv.size() < 6) rest = sv;
                    if (string_starts_with("null", rest) || string_starts_with("true", rest) || string_starts_with("false", rest) || std::regex_match(sv.begin(), sv.end(), number_regex)) {
                        value_json = {123, {"123", "123"}};
                        builder.consume_rest();
                    } else {
                        builder.move_to(val_start);
                    }
                }
            }

            // If it is a JSON and followed by </arg_value>, parse as json
            // cannot support streaming because it may be a plain text starting with JSON
            if (value_json) {
                auto json_end = builder.pos();
                builder.consume_spaces();
                if (builder.pos() == builder.input().size()) {
                    if (form.raw_argval && !*form.raw_argval && (value_json->json.is_string() || value_json->json.is_object() || value_json->json.is_array())) {
                        arguments[key] = value_json->json;
                        auto json_str = arguments.dump();
                        if (!value_json->healing_marker.json_dump_marker.empty()) {
                            GGML_ASSERT(std::string::npos != json_str.rfind(value_json->healing_marker.json_dump_marker));
                            json_str.resize(json_str.rfind(value_json->healing_marker.json_dump_marker));
                        } else {
                            GGML_ASSERT(json_str.back() == '}');
                            json_str.resize(json_str.size() - 1);
                        }
                        builder.add_tool_call(function_name, "", json_str);
                    } else {
                        gen_partial_args([&](auto &, auto &needle) {arguments[key] = needle;});
                    }
                    LOG_DBG("Possible JSON arg_value: %s\n", value_json->json.dump().c_str());
                    throw common_chat_msg_partial_exception("JSON arg_value detected. Waiting for more tokens for validations.");
                }
                builder.move_to(json_end);
                auto [val_end_size, tc] = try_find_val_end();
                if (tc && all_space(tc->prelude) && value_json->healing_marker.marker.empty()) {
                    if (tc->groups[0].end - tc->groups[0].begin != val_end_size) {
                        gen_partial_args([&](auto &, auto &needle) {arguments[key] = needle;});
                        LOG_DBG("Possible terminated JSON arg_value: %s\n", value_json->json.dump().c_str());
                        throw common_chat_msg_partial_exception("Partial literal: " + gbnf_format_literal(form.val_end) + (form.last_val_end ? gbnf_format_literal(*form.last_val_end) : ""));
                    } else arguments[key] = value_json->json;
                } else builder.move_to(val_start);
            }

            // If not, parse as plain text
            if (val_start == builder.pos()) {
                if (auto [val_end_size, value_plain] = try_find_val_end(); value_plain) {
                    auto &value_str = value_plain->prelude;
                    if (form.trim_raw_argval) value_str = string_strip(value_str);
                    if (value_plain->groups[0].end - value_plain->groups[0].begin != val_end_size) {
                        gen_partial_args([&](auto &, auto &needle) {arguments[key] = value_str + needle;});
                        throw common_chat_msg_partial_exception(
                                "Expected " + gbnf_format_literal(form.val_end) +
                                " after " + gbnf_format_literal(form.key_val_sep) +
                                (form.key_val_sep2 ? " " + gbnf_format_literal(*form.key_val_sep2) : "")
                        );
                    }
                    arguments[key] = value_str;
                } else {
                    if (form.trim_raw_argval) {
                        gen_partial_args([&](auto &rest, auto &needle) {arguments[key] = string_strip(rest) + needle;});
                    } else {
                        gen_partial_args([&](auto &rest, auto &needle) {arguments[key] = rest + needle;});
                    }
                    throw common_chat_msg_partial_exception(
                            "Expected " + gbnf_format_literal(form.val_end) +
                            " after " + gbnf_format_literal(form.key_val_sep) +
                            (form.key_val_sep2 ? " " + gbnf_format_literal(*form.key_val_sep2) : "")
                    );
                }
            }
        }

        // Consume closing tag
        if (auto [tool_end_size, tc] = try_find_tool_end(); tc) {
            if (!all_space(tc->prelude)) {
                LOG_DBG("Failed to parse XML-Style tool call: Expected %s, but found %s\n",
                        gbnf_format_literal(form.tool_end).c_str(),
                        gbnf_format_literal(tc->prelude).c_str()
                );
                return return_error(builder, start_pos, recovery);
            }
            if (tc->groups[0].end - tc->groups[0].begin == tool_end_size) {
                // Add the parsed tool call
                if (!builder.add_tool_call(function_name, "", arguments.dump())) {
                    throw common_chat_msg_partial_exception("Failed to add XML-Style tool call");
                }
                recovery = false;
                continue;
            }
        }

        auto tool_call_arg = arguments.dump();
        if (tool_call_arg.size() != 0 && tool_call_arg[tool_call_arg.size() - 1] == '}') {
            tool_call_arg.resize(tool_call_arg.size() - 1);
        }
        builder.add_tool_call(function_name, "", tool_call_arg);
        throw common_chat_msg_partial_exception("Expected " + gbnf_format_literal(form.tool_end) + " after " + gbnf_format_literal(form.val_end));
    }
    if (auto tc = builder.try_find_literal(form.scope_end)) {
        if (!all_space(tc->prelude)) {
            LOG_DBG("Failed to parse XML-Style tool call: Expected %s, but found %s\n",
                    gbnf_format_literal(form.scope_end).c_str(),
                    gbnf_format_literal(tc->prelude).c_str()
            );
            return return_error(builder, start_pos, recovery);
        }
    } else {
        if (all_space(form.scope_end)) return true;
        builder.consume_spaces();
        if (builder.pos() == builder.input().size())
            throw common_chat_msg_partial_exception("incomplete tool calls");
        LOG_DBG("Failed to parse XML-Style tool call: Expected %s, but found %s\n",
                gbnf_format_literal(form.scope_end).c_str(),
                gbnf_format_literal(builder.consume_rest()).c_str()
        );
        return return_error(builder, start_pos, recovery);
    }

    return true;
}

/**
 * Parse XML-Style tool call for given xml_tool_call_format. Return false for invalid syntax and get the position untouched.
 * May cause std::runtime_error if there is invalid syntax because partial valid tool call is already sent out to client.
 * form.scope_start, form.tool_sep and form.scope_end can be empty.
 */
bool common_chat_msg_parser::try_consume_xml_tool_calls(const struct xml_tool_call_format & form) {
    auto pos = pos_;
    auto tsize = result_.tool_calls.size();
    try { return parse_xml_tool_calls(*this, form); }
    catch (const xml_toolcall_syntax_exception&) {}
    move_to(pos);
    result_.tool_calls.resize(tsize);
    return false;
}

/**
 * Parse content uses reasoning and XML-Style tool call
 * TODO: Note that form.allow_toolcall_in_think is not tested yet. If anyone confirms it works, this comment can be removed.
 */
inline void parse_msg_with_xml_tool_calls(common_chat_msg_parser & builder, const struct xml_tool_call_format & form, const std::string & start_think = "<think>", const std::string & end_think = "</think>") {
    constexpr auto rstrip = [](std::string &s) {
        s.resize(std::distance(s.begin(), std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base()));
    };
    // Erase substring from l to r, along with additional spaces nearby
    constexpr auto erase_spaces = [](auto &str, size_t l, size_t r) {
        while (/* l > -1 && */ --l < str.size() && std::isspace(static_cast<unsigned char>(str[l])));
        ++l;
        while (++r < str.size() && std::isspace(static_cast<unsigned char>(str[r])));
        if (l < r) str[l] = '\n';
        if (l + 1 < r) str[l + 1] = '\n';
        if (l != 0) l += 2;
        str.erase(l, r - l);
        return l;
    };
    constexpr auto trim_suffix = [](std::string &content, std::initializer_list<std::string_view> list) {
        auto best_match = content.size();
        for (auto pattern: list) {
            if (pattern.size() == 0) continue;
            for (auto match_idx = content.size() - std::min(pattern.size(), content.size()); content.size() > match_idx; match_idx++) {
                auto match_len = content.size() - match_idx;
                if (content.compare(match_idx, match_len, pattern.data(), match_len) == 0 && best_match > match_idx) {
                    best_match = match_idx;
                }
            }
        }
        if (content.size() > best_match) {
            content.erase(best_match);
        }
    };
    const auto trim_potential_partial_word = [&start_think, &end_think, &form, trim_suffix](std::string &content) {
        return trim_suffix(content, {
            start_think, end_think, form.scope_start, form.tool_start, form.tool_sep, form.key_start,
            form.key_val_sep, form.key_val_sep2 ? form.key_val_sep2->c_str() : "",
            form.val_end, form.last_val_end ? form.last_val_end->c_str() : "",
            form.tool_end, form.last_tool_end ? form.last_tool_end->c_str() : "",
            form.scope_end
        });
    };


    // Trim leading spaces without affecting keyword matching
    static const common_regex spaces_regex("\\s*");
    {
        auto tc = builder.consume_regex(spaces_regex);
        auto spaces = builder.str(tc.groups[0]);
        auto s1 = spaces.size();
        trim_potential_partial_word(spaces);
        auto s2 = spaces.size();
        builder.move_to(builder.pos() - (s1 - s2));
    }

    // Parse content
    bool reasoning_unclosed = builder.syntax().thinking_forced_open;
    std::string unclosed_reasoning_content("");
    for (;;) {
        auto tc = try_find_2_literal_splited_by_spaces(builder, form.scope_start, form.tool_start);
        std::string content;
        std::string tool_call_start;

        if (tc) {
            content = std::move(tc->prelude);
            tool_call_start = builder.str(tc->groups[0]);
            LOG_DBG("Matched tool start: %s\n", gbnf_format_literal(tool_call_start).c_str());
        } else {
            content = builder.consume_rest();
            utf8_truncate_safe_resize(content);
        }

        // Handle unclosed think block
        if (reasoning_unclosed) {
            if (auto pos = content.find(end_think); pos == std::string::npos && builder.pos() != builder.input().size()) {
                unclosed_reasoning_content += content;
                if (!(form.allow_toolcall_in_think && tc)) {
                    unclosed_reasoning_content += tool_call_start;
                    continue;
                }
            } else {
                reasoning_unclosed = false;
                std::string reasoning_content;
                if (pos == std::string::npos) {
                    reasoning_content = std::move(content);
                } else {
                    reasoning_content = content.substr(0, pos);
                    content.erase(0, pos + end_think.size());
                }
                if (builder.pos() == builder.input().size() && all_space(content)) {
                    rstrip(reasoning_content);
                    trim_potential_partial_word(reasoning_content);
                    rstrip(reasoning_content);
                    if (reasoning_content.empty()) {
                        rstrip(unclosed_reasoning_content);
                        trim_potential_partial_word(unclosed_reasoning_content);
                        rstrip(unclosed_reasoning_content);
                        if (unclosed_reasoning_content.empty()) continue;
                    }
                }
                if (builder.syntax().reasoning_format == COMMON_REASONING_FORMAT_NONE || builder.syntax().reasoning_in_content) {
                    builder.add_content(start_think);
                    builder.add_content(unclosed_reasoning_content);
                    builder.add_content(reasoning_content);
                    if (builder.pos() != builder.input().size() || !all_space(content))
                        builder.add_content(end_think);
                } else {
                    builder.add_reasoning_content(unclosed_reasoning_content);
                    builder.add_reasoning_content(reasoning_content);
                }
                unclosed_reasoning_content.clear();
            }
        }

        // Handle multiple think block
        bool toolcall_in_think = false;
        for (auto think_start = content.find(start_think); think_start != std::string::npos; think_start = content.find(start_think, think_start)) {
            if (auto think_end = content.find(end_think, think_start + start_think.size()); think_end != std::string::npos) {
                if (builder.syntax().reasoning_format != COMMON_REASONING_FORMAT_NONE && !builder.syntax().reasoning_in_content) {
                    auto reasoning_content = content.substr(think_start + start_think.size(), think_end - think_start - start_think.size());
                    builder.add_reasoning_content(reasoning_content);
                    think_start = erase_spaces(content, think_start, think_end + end_think.size() - 1);
                } else {
                    think_start = think_end + end_think.size() - 1;
                }
            } else {
                // This <tool_call> start is in thinking block, skip this tool call
                // This <tool_call> start is in thinking block
                if (form.allow_toolcall_in_think) {
                    unclosed_reasoning_content = content.substr(think_start + start_think.size());
                } else {
                    unclosed_reasoning_content = content.substr(think_start + start_think.size()) + tool_call_start;
                }
                reasoning_unclosed = true;
                content.resize(think_start);
                toolcall_in_think = true;
            }
        }

        if (builder.syntax().reasoning_format != COMMON_REASONING_FORMAT_NONE && !builder.syntax().reasoning_in_content) {
            rstrip(content);
            // Handle unclosed </think> token from content: delete all </think> token
            if (auto pos = content.rfind(end_think); pos != std::string::npos) {
                while (pos != std::string::npos) {
                    pos = erase_spaces(content, pos, pos + end_think.size() - 1);
                    pos = content.rfind(end_think, pos);
                }
            }
            // Strip if needed
            if (content.size() > 0 && std::isspace(static_cast<unsigned char>(content[0]))) {
                content = string_strip(content);
            }
        }

        // remove potential partial suffix
        if (builder.pos() == builder.input().size()) {
            if (unclosed_reasoning_content.empty()) {
                rstrip(content);
                trim_potential_partial_word(content);
                rstrip(content);
            } else {
                rstrip(unclosed_reasoning_content);
                trim_potential_partial_word(unclosed_reasoning_content);
                rstrip(unclosed_reasoning_content);
            }
        }

        // consume unclosed_reasoning_content if allow_toolcall_in_think is set
        if (form.allow_toolcall_in_think && !unclosed_reasoning_content.empty()) {
            if (builder.syntax().reasoning_format != COMMON_REASONING_FORMAT_NONE && !builder.syntax().reasoning_in_content) {
                builder.add_reasoning_content(unclosed_reasoning_content);
            } else {
                if (content.empty()) {
                    content = start_think + unclosed_reasoning_content;
                } else {
                    content += "\n\n" + start_think;
                    content += unclosed_reasoning_content;
                }
            }
            unclosed_reasoning_content.clear();
        }

        // Add content
        if (!content.empty()) {
            // If there are multiple content blocks
            if (builder.syntax().reasoning_format != COMMON_REASONING_FORMAT_NONE && !builder.syntax().reasoning_in_content && builder.result().content.size() != 0) {
                builder.add_content("\n\n");
            }
            builder.add_content(content);
        }

        // This <tool_call> start is in thinking block and toolcall_in_think not set, skip this tool call
        if (toolcall_in_think && !form.allow_toolcall_in_think) {
            continue;
        }

        // There is no tool call and all content is parsed
        if (!tc) {
            GGML_ASSERT(builder.pos() == builder.input().size());
            GGML_ASSERT(unclosed_reasoning_content.empty());
            if (!form.allow_toolcall_in_think) GGML_ASSERT(!reasoning_unclosed);
            break;
        }

        builder.move_to(tc->groups[0].begin);
        if (builder.try_consume_xml_tool_calls(form)) {
            auto end_of_tool = builder.pos();
            builder.consume_spaces();
            if (builder.pos() != builder.input().size()) {
                builder.move_to(end_of_tool);
                if (!builder.result().content.empty()) {
                    builder.add_content("\n\n");
                }
            }
        } else {
            static const common_regex next_char_regex(".");
            auto c = builder.str(builder.consume_regex(next_char_regex).groups[0]);
            rstrip(c);
            builder.add_content(c);
        }
    }
}

/**
 * Parse content uses reasoning and XML-Style tool call
 */
void common_chat_msg_parser::consume_reasoning_with_xml_tool_calls(const struct xml_tool_call_format & form, const std::string & start_think, const std::string & end_think) {
    parse_msg_with_xml_tool_calls(*this, form, start_think, end_think);
}
 45 changes: 0 additions & 45 deletions45  
common/chat-parser-xml-toolcall.h
Original file line number	Diff line number	Diff line change
@@ -1,45 +0,0 @@
#pragma once

#include "chat.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>


// Sample config:
// MiniMax-M2 (left): <minimax:tool_call>\n<invoke name="tool-name">\n<parameter name="key">value</parameter>\n...</invoke>\n...</minimax:tool_call>
// GLM 4.5   (right): <tool_call>function_name\n<arg_key>key</arg_key>\n<arg_value>value</arg_value>\n</tool_call>
struct xml_tool_call_format {
    std::string scope_start; // <minimax:tool_call>\n  // \n                      // can be empty
    std::string tool_start;  // <invoke name=\"        // <tool_call>
    std::string tool_sep;    // \">\n                  // \n                      // can be empty only for parse_xml_tool_calls
    std::string key_start;   // <parameter name=\"     // <arg_key>
    std::string key_val_sep; // \">                    // </arg_key>\n<arg_value>
    std::string val_end;     // </parameter>\n         // </arg_value>\n
    std::string tool_end;    // </invoke>\n            // </tool_call>\n
    std::string scope_end;   // </minimax:tool_call>   //                         // can be empty
    // Set this if there can be dynamic spaces inside key_val_sep.
    // e.g. key_val_sep=</arg_key> key_val_sep2=<arg_value> for GLM4.5
    std::optional<std::string> key_val_sep2 = std::nullopt;
    // Set true if argval should only be raw string. e.g. Hello "world" hi
    // Set false if argval should only be json string. e.g. "Hello \"world\" hi"
    // Defaults to std::nullopt, both will be allowed.
    std::optional<bool> raw_argval = std::nullopt;
    std::optional<std::string> last_val_end = std::nullopt;
    std::optional<std::string> last_tool_end = std::nullopt;
    bool trim_raw_argval = false;
    bool allow_toolcall_in_think = false;
};

// make a GBNF that accept any strings except those containing any of the forbidden strings.
std::string make_gbnf_excluding(std::vector<std::string> forbids);

/**
 * Build grammar for xml-style tool call
 * form.scope_start and form.scope_end can be empty.
 * Requires data.format for model-specific hacks.
 */
void build_grammar_xml_tool_call(common_chat_params & data, const nlohmann::ordered_json & tools, const struct xml_tool_call_format & form);
 323 changes: 180 additions & 143 deletions323  
common/chat-parser.cpp
Original file line number	Diff line number	Diff line change
@@ -879,93 +879,6 @@ static void common_chat_parse_deepseek_v3_1(common_chat_msg_parser & builder) {
    }
}

static void common_chat_parse_minimax_m2(common_chat_msg_parser & builder) {
    static const xml_tool_call_format form {
        /* form.scope_start = */ "<minimax:tool_call>",
        /* form.tool_start  = */ "<invoke name=\"",
        /* form.tool_sep    = */ "\">",
        /* form.key_start   = */ "<parameter name=\"",
        /* form.key_val_sep = */ "\">",
        /* form.val_end     = */ "</parameter>",
        /* form.tool_end    = */ "</invoke>",
        /* form.scope_end   = */ "</minimax:tool_call>",
    };
    builder.consume_reasoning_with_xml_tool_calls(form, "<think>", "</think>");
}

static void common_chat_parse_qwen3_coder_xml(common_chat_msg_parser & builder) {
    static const xml_tool_call_format form = ([]() {
        xml_tool_call_format form {};
        form.scope_start = "<tool_call>";
        form.tool_start  = "<function=";
        form.tool_sep    = ">";
        form.key_start   = "<parameter=";
        form.key_val_sep = ">";
        form.val_end     = "</parameter>";
        form.tool_end    = "</function>";
        form.scope_end   = "</tool_call>";
        form.trim_raw_argval = true;
        return form;
    })();
    builder.consume_reasoning_with_xml_tool_calls(form);
}

static void common_chat_parse_kimi_k2(common_chat_msg_parser & builder) {
    static const xml_tool_call_format form = ([]() {
        xml_tool_call_format form {};
        form.scope_start = "<|tool_calls_section_begin|>";
        form.tool_start  = "<|tool_call_begin|>";
        form.tool_sep    = "<|tool_call_argument_begin|>{";
        form.key_start   = "\"";
        form.key_val_sep = "\":";
        form.val_end     = ",";
        form.tool_end    = "}<|tool_call_end|>";
        form.scope_end   = "<|tool_calls_section_end|>";
        form.raw_argval  = false;
        form.last_val_end = "";
        form.allow_toolcall_in_think = true;
        return form;
    })();
    builder.consume_reasoning_with_xml_tool_calls(form, "<think>", "</think>");
}

static void common_chat_parse_apriel_1_5(common_chat_msg_parser & builder) {
    static const xml_tool_call_format form = ([]() {
        xml_tool_call_format form {};
        form.scope_start = "<tool_calls>[";
        form.tool_start  = "{\"name\": \"";
        form.tool_sep    = "\", \"arguments\": {";
        form.key_start   = "\"";
        form.key_val_sep = "\": ";
        form.val_end     = ", ";
        form.tool_end    = "}, ";
        form.scope_end   = "]</tool_calls>";
        form.raw_argval  = false;
        form.last_val_end = "";
        form.last_tool_end = "}";
        return form;
    })();
    builder.consume_reasoning_with_xml_tool_calls(form, "<thinking>", "</thinking>");
}

static void common_chat_parse_xiaomi_mimo(common_chat_msg_parser & builder) {
    static const xml_tool_call_format form = ([]() {
        xml_tool_call_format form {};
        form.scope_start = "";
        form.tool_start  = "<tool_call>\n{\"name\": \"";
        form.tool_sep    = "\", \"arguments\": {";
        form.key_start   = "\"";
        form.key_val_sep = "\": ";
        form.val_end     = ", ";
        form.tool_end    = "}\n</tool_call>";
        form.scope_end   = "";
        form.raw_argval  = false;
        form.last_val_end = "";
        return form;
    })();
    builder.consume_reasoning_with_xml_tool_calls(form);
}

static void common_chat_parse_gpt_oss(common_chat_msg_parser & builder) {
    static const std::string constraint = "(?: (<\\|constrain\\|>)?([a-zA-Z0-9_-]+))";
    static const std::string recipient("(?: to=functions\\.([^<\\s]+))");
@@ -1054,21 +967,6 @@ static void common_chat_parse_gpt_oss(common_chat_msg_parser & builder) {
    }
}

static void common_chat_parse_glm_4_5(common_chat_msg_parser & builder) {
    static const xml_tool_call_format form {
        /* form.scope_start  = */ "",
        /* form.tool_start   = */ "<tool_call>",
        /* form.tool_sep     = */ "",
        /* form.key_start    = */ "<arg_key>",
        /* form.key_val_sep  = */ "</arg_key>",
        /* form.val_end      = */ "</arg_value>",
        /* form.tool_end     = */ "</tool_call>",
        /* form.scope_end    = */ "",
        /* form.key_val_sep2 = */ "<arg_value>",
    };
    builder.consume_reasoning_with_xml_tool_calls(form, "<think>", "</think>");
}

static void common_chat_parse_firefunction_v2(common_chat_msg_parser & builder) {
    if (!builder.syntax().parse_tool_calls) {
        builder.add_content(builder.consume_rest());
@@ -1381,18 +1279,145 @@ static void common_chat_parse_lfm2(common_chat_msg_parser & builder) {
    }
}

static void common_chat_parse_seed_oss(common_chat_msg_parser & builder) {
    static const xml_tool_call_format form {
        /* form.scope_start = */ "<seed:tool_call>",
        /* form.tool_start  = */ "<function=",
        /* form.tool_sep    = */ ">",
        /* form.key_start   = */ "<parameter=",
        /* form.key_val_sep = */ ">",
        /* form.val_end     = */ "</parameter>",
        /* form.tool_end    = */ "</function>",
        /* form.scope_end   = */ "</seed:tool_call>",
    };
    builder.consume_reasoning_with_xml_tool_calls(form, "<seed:think>", "</seed:think>");
// FunctionGemma format: <start_function_call>call:name{key:<escape>value<escape>,key2:123}<end_function_call>
// Helper to find the closing brace of a FunctionGemma call, accounting for <escape> delimiters
static size_t find_function_gemma_args_end(const std::string & input, size_t start) {
    bool in_escape = false;
    for (size_t i = start; i < input.size(); ++i) {
        if (!in_escape && input.substr(i, 8) == "<escape>") {
            in_escape = true;
            i += 7; // Skip to end of "<escape>" (loop will add 1)
        } else if (in_escape && input.substr(i, 8) == "<escape>") {
            in_escape = false;
            i += 7; // Skip to end of "</escape>"
        } else if (!in_escape && input[i] == '}') {
            return i;
        }
    }
    return std::string::npos;
}

static void common_chat_parse_function_gemma(common_chat_msg_parser & builder) {
    if (!builder.syntax().parse_tool_calls) {
        builder.add_content(builder.consume_rest());
        return;
    }

    // Match the start of a function call: <start_function_call>call:name{
    static const common_regex tool_call_start_regex(
        "<start_function_call>call:([a-zA-Z_][a-zA-Z0-9_]*)\\{");

    while (true) {
        auto res = builder.try_find_regex(tool_call_start_regex);
        if (!res) {
            // No more tool calls found, consume rest as content
            auto remaining = builder.consume_rest();
            if (!remaining.empty()) {
                builder.add_content(remaining);
            }
            break;
        }

        // Extract function name
        std::string function_name = builder.str(res->groups[1]);

        // Find the closing brace, accounting for <escape> delimiters
        const std::string & input = builder.input();
        size_t args_start = builder.pos();
        size_t args_end = find_function_gemma_args_end(input, args_start);

        if (args_end == std::string::npos) {
            // Incomplete - no closing brace found
            throw common_chat_msg_partial_exception("Incomplete FunctionGemma tool call - no closing brace");
        }

        std::string args_str = input.substr(args_start, args_end - args_start);
        builder.move_to(args_end + 1); // Move past the closing brace

        // Consume the end tag
        static const std::string end_tag = "<end_function_call>";
        if (input.substr(builder.pos(), end_tag.size()) == end_tag) {
            builder.move_to(builder.pos() + end_tag.size());
        }

        // Parse the arguments: key:<escape>value<escape> or key:value
        json arguments = json::object();

        size_t pos = 0;
        while (pos < args_str.size()) {
            // Skip leading whitespace and commas
            while (pos < args_str.size() && (std::isspace(args_str[pos]) || args_str[pos] == ',')) {
                ++pos;
            }
            if (pos >= args_str.size()) break;

            // Find the key (ends at ':')
            size_t key_end = args_str.find(':', pos);
            if (key_end == std::string::npos) break;

            std::string key = args_str.substr(pos, key_end - pos);
            // Trim key
            while (!key.empty() && std::isspace(key.front())) key.erase(0, 1);
            while (!key.empty() && std::isspace(key.back())) key.pop_back();

            pos = key_end + 1;

            // Check if value is escaped (string) or raw
            std::string value;
            bool is_string = false;

            // Skip whitespace after colon
            while (pos < args_str.size() && std::isspace(args_str[pos])) {
                ++pos;
            }

            if (pos < args_str.size() && args_str.substr(pos, 8) == "<escape>") {
                // String value wrapped in <escape>...</escape>
                is_string = true;
                pos += 8; // Skip "<escape>"
                size_t val_end = args_str.find("<escape>", pos);
                if (val_end == std::string::npos) {
                    // Partial value - take rest
                    value = args_str.substr(pos);
                    pos = args_str.size();
                } else {
                    value = args_str.substr(pos, val_end - pos);
                    pos = val_end + 8; // Skip closing "<escape>"
                }
            } else {
                // Raw value (number, boolean, etc.) - ends at comma or end
                size_t val_end = args_str.find(',', pos);
                if (val_end == std::string::npos) {
                    value = args_str.substr(pos);
                    pos = args_str.size();
                } else {
                    value = args_str.substr(pos, val_end - pos);
                    pos = val_end;
                }
                // Trim value
                while (!value.empty() && std::isspace(value.back())) value.pop_back();
            }

            // Add to arguments JSON
            if (!key.empty()) {
                if (is_string) {
                    arguments[key] = value;
                } else {
                    // Try to parse as JSON value (number, boolean, null)
                    try {
                        arguments[key] = json::parse(value);
                    } catch (...) {
                        // If parsing fails, treat as string
                        arguments[key] = value;
                    }
                }
            }
        }

        if (!builder.add_tool_call(function_name, "", arguments.dump())) {
            throw common_chat_msg_partial_exception("Incomplete FunctionGemma tool call");
        }
    }
}

static void common_chat_parse_content_only(common_chat_msg_parser & builder) {
@@ -1401,7 +1426,7 @@ static void common_chat_parse_content_only(common_chat_msg_parser & builder) {
}

static void common_chat_parse(common_chat_msg_parser & builder) {
    LOG_DBG("Parsing input with format %s: %s\n", common_chat_format_name(builder.syntax().format), builder.input().c_str());
    LOG_INF("Parsing input with format %s: %s\n", common_chat_format_name(builder.syntax().format), builder.input().c_str());

    switch (builder.syntax().format) {
        case COMMON_CHAT_FORMAT_CONTENT_ONLY:
@@ -1449,9 +1474,6 @@ static void common_chat_parse(common_chat_msg_parser & builder) {
        case COMMON_CHAT_FORMAT_GPT_OSS:
            common_chat_parse_gpt_oss(builder);
            break;
        case COMMON_CHAT_FORMAT_SEED_OSS:
            common_chat_parse_seed_oss(builder);
            break;
        case COMMON_CHAT_FORMAT_NEMOTRON_V2:
            common_chat_parse_nemotron_v2(builder);
            break;
@@ -1461,23 +1483,18 @@ static void common_chat_parse(common_chat_msg_parser & builder) {
        case COMMON_CHAT_FORMAT_LFM2_WITH_JSON_TOOLS:
            common_chat_parse_lfm2(builder);
            break;
        case COMMON_CHAT_FORMAT_MINIMAX_M2:
            common_chat_parse_minimax_m2(builder);
        case COMMON_CHAT_FORMAT_FUNCTION_GEMMA:
            common_chat_parse_function_gemma(builder);
            break;
        // Formats with on-demand PEG parsers: when called without parser, fall back to content-only
        case COMMON_CHAT_FORMAT_SEED_OSS:
        case COMMON_CHAT_FORMAT_MINIMAX_M2:
        case COMMON_CHAT_FORMAT_GLM_4_5:
            common_chat_parse_glm_4_5(builder);
            break;
        case COMMON_CHAT_FORMAT_KIMI_K2:
            common_chat_parse_kimi_k2(builder);
            break;
        case COMMON_CHAT_FORMAT_QWEN3_CODER_XML:
            common_chat_parse_qwen3_coder_xml(builder);
            break;
        case COMMON_CHAT_FORMAT_APRIEL_1_5:
            common_chat_parse_apriel_1_5(builder);
            break;
        case COMMON_CHAT_FORMAT_QWEN3_CODER_XML:
        case COMMON_CHAT_FORMAT_XIAOMI_MIMO:
            common_chat_parse_xiaomi_mimo(builder);
            common_chat_parse_content_only(builder);
            break;
        default:
            throw std::runtime_error(std::string("Unsupported format: ") + common_chat_format_name(builder.syntax().format));
@@ -1486,11 +1503,12 @@ static void common_chat_parse(common_chat_msg_parser & builder) {
}

common_chat_msg common_chat_parse(const std::string & input, bool is_partial, const common_chat_syntax & syntax) {
    if (syntax.format == COMMON_CHAT_FORMAT_PEG_SIMPLE ||
        syntax.format == COMMON_CHAT_FORMAT_PEG_NATIVE ||
        syntax.format == COMMON_CHAT_FORMAT_PEG_CONSTRUCTED) {
    // If a PEG parser is available, use it (this is the preferred path - always provide a parser)
    if (!syntax.parser.empty()) {
        return common_chat_peg_parse(syntax.parser, input, is_partial, syntax);
    }

    // Legacy non-PEG parsing path for older formats (deprecated - prefer using PEG parser)
    common_chat_msg_parser builder(input, is_partial, syntax);
    try {
        common_chat_parse(builder);
@@ -1514,7 +1532,7 @@ common_chat_msg common_chat_peg_parse(const common_peg_arena & parser, const std
        throw std::runtime_error("Failed to parse due to missing parser definition.");
    }

    LOG_DBG("Parsing input with format %s: %s\n", common_chat_format_name(syntax.format), input.c_str());
    LOG_INF("Parsing input with format %s: %s\n", common_chat_format_name(syntax.format), input.c_str());

    common_peg_parse_context ctx(input, is_partial);
    auto result = parser.parse(ctx);
@@ -1525,16 +1543,35 @@ common_chat_msg common_chat_peg_parse(const common_peg_arena & parser, const std
    common_chat_msg msg;
    msg.role = "assistant";

    if (syntax.format == COMMON_CHAT_FORMAT_PEG_NATIVE) {
        auto mapper = common_chat_peg_native_mapper(msg);
        mapper.from_ast(ctx.ast, result);
    } else if (syntax.format == COMMON_CHAT_FORMAT_PEG_CONSTRUCTED) {
        auto mapper = common_chat_peg_constructed_mapper(msg);
        mapper.from_ast(ctx.ast, result);
    // Select mapper based on format
    // - constructed_mapper: XML-style formats with arg key/value pairs
    // - function_gemma_mapper: FunctionGemma's custom format
    // - short_form_mapper: Apertus short-form tool calls
    // - native_mapper: JSON-based formats (default)
    if (syntax.format == COMMON_CHAT_FORMAT_NEMOTRON_V3 ||
        syntax.format == COMMON_CHAT_FORMAT_SEED_OSS ||
        syntax.format == COMMON_CHAT_FORMAT_MINIMAX_M2 ||
        syntax.format == COMMON_CHAT_FORMAT_QWEN3_CODER_XML ||
        syntax.format == COMMON_CHAT_FORMAT_GLM_4_5 ||
        syntax.format == COMMON_CHAT_FORMAT_LLAMA_3_X_WITH_BUILTIN_TOOLS) {
        apply_chat_peg_mapper(common_chat_peg_constructed_mapper(), ctx.ast, result, msg);
    } else if (syntax.format == COMMON_CHAT_FORMAT_FUNCTION_GEMMA) {
        apply_chat_peg_mapper(common_chat_peg_function_gemma_mapper(), ctx.ast, result, msg);
    } else if (syntax.format == COMMON_CHAT_FORMAT_APERTUS ||
               syntax.format == COMMON_CHAT_FORMAT_APRIEL_1_5) {
        apply_chat_peg_mapper(common_chat_peg_short_form_mapper(), ctx.ast, result, msg);
    } else if (syntax.format == COMMON_CHAT_FORMAT_COMMAND_R7B) {
        apply_chat_peg_mapper(common_chat_peg_command_r7b_mapper(), ctx.ast, result, msg);
    } else if (syntax.format == COMMON_CHAT_FORMAT_GENERIC) {
        apply_chat_peg_mapper(common_chat_peg_generic_mapper(), ctx.ast, result, msg);
    } else if (syntax.format == COMMON_CHAT_FORMAT_MISTRAL_NEMO ||
               syntax.format == COMMON_CHAT_FORMAT_MAGISTRAL ||
               syntax.format == COMMON_CHAT_FORMAT_FIREFUNCTION_V2 ||
               syntax.format == COMMON_CHAT_FORMAT_NEMOTRON_V2 ||
               syntax.format == COMMON_CHAT_FORMAT_GRANITE) {
        apply_chat_peg_mapper(common_chat_peg_oai_array_mapper(), ctx.ast, result, msg);
    } else {
        // Generic mapper
        auto mapper = common_chat_peg_mapper(msg);
        mapper.from_ast(ctx.ast, result);
        // Default to native mapper for JSON-based formats (including KIMI_K2, XIAOMI_MIMO)
        apply_chat_peg_mapper(common_chat_peg_native_mapper(), ctx.ast, result, msg);
    }
    if (!is_partial) {
        LOG_DBG("Parsed message: %s\n", common_chat_msgs_to_json_oaicompat<json>({msg}).at(0).dump().c_str());
  10 changes: 0 additions & 10 deletions10  
common/chat-parser.h
Original file line number	Diff line number	Diff line change
@@ -1,7 +1,6 @@
#pragma once

#include "chat.h"
#include "chat-parser-xml-toolcall.h"
#include "json-partial.h"
#include "regex-partial.h"

@@ -120,14 +119,5 @@ class common_chat_msg_parser {
        const std::vector<std::vector<std::string>> & content_paths = {}
    );

    /**
     * Parse XML-Style tool call for given xml_tool_call_format. Return false for invalid syntax and get the position untouched.
     * form.scope_start, form.tool_sep and form.scope_end can be empty.
     */
    bool try_consume_xml_tool_calls(const struct xml_tool_call_format & form);

    // Parse content uses reasoning and XML-Style tool call
    void consume_reasoning_with_xml_tool_calls(const struct xml_tool_call_format & form, const std::string & start_think = "<think>", const std::string & end_think = "</think>");

    void clear_tools();
};
 167 changes: 167 additions & 0 deletions167  
common/chat-parsers-internal.h
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,167 @@
#pragma once

// Internal header for chat template format implementations.
// This header is NOT part of the public API and should only be included by:
// - common/chat.cpp (main implementation)
// - common/chat-parsers/*.cpp (per-format implementations)

#include "chat.h"
#include "chat-parser.h"
#include "chat-peg-parser.h"
#include "common.h"
#include "json-schema-to-grammar.h"
#include "regex-partial.h"

#include <minja/chat-template.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <string>

// JSON type alias
using json = nlohmann::ordered_json;

// Template type alias (from minja)
typedef minja::chat_template common_chat_template;

// Parameters for template-based format initialization functions
struct templates_params {
    json messages;
    json tools;
    common_chat_tool_choice tool_choice;
    json json_schema;
    bool parallel_tool_calls;
    common_reasoning_format reasoning_format;
    bool stream;
    std::string grammar;
    bool add_generation_prompt = true;
    bool enable_thinking = true;
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    json extra_context;
    bool add_bos;
    bool add_eos;
    bool is_inference = true;
};

// Helper to iterate over function tools
inline void foreach_function(const json & tools, const std::function<void(const json &)> & fn) {
    for (const auto & tool : tools) {
        if (!tool.contains("type") || tool.at("type") != "function" || !tool.contains("function")) {
            continue;
        }
        fn(tool);
    }
}

// Helper to iterate over function parameters
inline void foreach_parameter(const json & function, const std::function<void(const std::string &, const json &, bool)> & fn) {
    if (!function.contains("parameters") || !function.at("parameters").is_object()) {
        return;
    }
    const auto & params = function.at("parameters");
    if (!params.contains("properties") || !params.at("properties").is_object()) {
        return;
    }
    const auto & props = params.at("properties");
    std::set<std::string> required;
    if (params.contains("required") && params.at("required").is_array()) {
        params.at("required").get_to(required);
    }
    for (const auto & [name, prop] : props.items()) {
        bool is_required = (required.find(name) != required.end());
        fn(name, prop, is_required);
    }
}

// Format time for template contexts
inline std::string format_time(const std::chrono::system_clock::time_point & now, const std::string & format) {
    auto time = std::chrono::system_clock::to_time_t(now);
    auto local_time = *std::localtime(&time);
    std::ostringstream ss;
    ss << std::put_time(&local_time, format.c_str());
    return ss.str();
}

// Apply chat template with inputs
inline std::string apply(
    const common_chat_template & tmpl,
    const struct templates_params & inputs,
    const std::optional<json> & messages_override = std::nullopt,
    const std::optional<json> & tools_override = std::nullopt,
    const std::optional<json> & additional_context = std::nullopt)
{
    minja::chat_template_inputs tmpl_inputs;
    tmpl_inputs.messages = messages_override ? *messages_override : inputs.messages;
    if (tools_override) {
        tmpl_inputs.tools = *tools_override;
    } else {
        tmpl_inputs.tools = inputs.tools.empty() ? json() : inputs.tools;
    }
    tmpl_inputs.add_generation_prompt = inputs.add_generation_prompt;
    tmpl_inputs.extra_context = inputs.extra_context;
    tmpl_inputs.extra_context["enable_thinking"] = inputs.enable_thinking;
    if (additional_context) {
        tmpl_inputs.extra_context.merge_patch(*additional_context);
    }

    minja::chat_template_options tmpl_opts;
    auto result = tmpl.apply(tmpl_inputs, tmpl_opts);
    if (inputs.add_bos && string_starts_with(result, tmpl.bos_token())) {
        result = result.substr(tmpl.bos_token().size());
    }
    if (inputs.add_eos && string_ends_with(result, tmpl.eos_token())) {
        result = result.substr(0, result.size() - tmpl.eos_token().size());
    }
    return result;
}

// Type for format initialization functions
typedef common_chat_params (*common_chat_format_init_fn)(
    const common_chat_template & tmpl,
    const struct templates_params & params
);

// Type for format initialization functions that need extra inputs
typedef common_chat_params (*common_chat_format_init_fn_with_inputs)(
    const common_chat_template & tmpl,
    const struct templates_params & params,
    const common_chat_templates_inputs & inputs
);

// Type for llama_3_x style init that takes extra bool
typedef common_chat_params (*common_chat_format_init_fn_llama3x)(
    const common_chat_template & tmpl,
    const struct templates_params & params,
    bool allow_python_tag_builtin_tools
);

// Forward declarations for modular parser implementations in chat-parsers/
common_chat_params common_chat_params_init_mistral_nemo(const common_chat_template & tmpl, const struct templates_params & inputs);
common_chat_params common_chat_params_init_magistral(const common_chat_template & tmpl, const struct templates_params & inputs);
common_chat_params common_chat_params_init_command_r7b(const common_chat_template & tmpl, const struct templates_params & inputs);
common_chat_params common_chat_params_init_deepseek_r1(const common_chat_template & tmpl, const struct templates_params & inputs);
common_chat_params common_chat_params_init_deepseek_v3_1(const common_chat_template & tmpl, const struct templates_params & inputs);
common_chat_params common_chat_params_init_firefunction_v2(const common_chat_template & tmpl, const struct templates_params & inputs);
common_chat_params common_chat_params_init_function_gemma(const common_chat_template & tmpl, const struct templates_params & inputs);
common_chat_params common_chat_params_init_hermes_2_pro(const common_chat_template & tmpl, const struct templates_params & inputs);
common_chat_params common_chat_params_init_llama_3_x(const common_chat_template & tmpl, const struct templates_params & inputs, bool allow_python_tag_builtin_tools);
common_chat_params common_chat_params_init_ministral_3(const common_chat_template & tmpl, const struct templates_params & inputs);
common_chat_params common_chat_params_init_nemotron_v3(const common_chat_template & tmpl, const struct templates_params & inputs);
common_chat_params common_chat_params_init_seed_oss(const common_chat_template & tmpl, const struct templates_params & inputs);
common_chat_params common_chat_params_init_nemotron_v2(const common_chat_template & tmpl, const struct templates_params & inputs);
common_chat_params common_chat_params_init_lfm2(const common_chat_template & tmpl, const struct templates_params & inputs);
common_chat_params common_chat_params_init_apertus(const common_chat_template & tmpl, const struct templates_params & inputs);
common_chat_params common_chat_params_init_minimax_m2(const common_chat_template & tmpl, const struct templates_params & inputs);
common_chat_params common_chat_params_init_qwen3_coder_xml(const common_chat_template & tmpl, const struct templates_params & inputs);
common_chat_params common_chat_params_init_kimi_k2(const common_chat_template & tmpl, const struct templates_params & inputs);
common_chat_params common_chat_params_init_apriel_1_5(const common_chat_template & tmpl, const struct templates_params & inputs);
common_chat_params common_chat_params_init_xiaomi_mimo(const common_chat_template & tmpl, const struct templates_params & inputs);
common_chat_params common_chat_params_init_glm_4_5(const common_chat_template & tmpl, const struct templates_params & inputs);
common_chat_params common_chat_params_init_granite(const common_chat_template & tmpl, const struct templates_params & inputs);
common_chat_params common_chat_params_init_functionary_v3_1_llama_3_1(const common_chat_template & tmpl, const struct templates_params & inputs);
common_chat_params common_chat_params_init_functionary_v3_2(const common_chat_template & tmpl, const struct templates_params & inputs);
common_chat_params common_chat_params_init_gpt_oss(const common_chat_template & tmpl, const struct templates_params & inputs);
common_chat_params common_chat_params_init_generic(const common_chat_template & tmpl, const struct templates_params & inputs);
 120 changes: 120 additions & 0 deletions120  
common/chat-parsers/apertus.cpp
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,120 @@
// Apertus tool call format
// Format: <|tools_prefix|>[{"func_name": {"arg1": value1}}]<|tools_suffix|>
// With optional <|inner_prefix|>...<|inner_suffix|> reasoning blocks

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_apertus(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_APERTUS;

    // Handle thinking tags appropriately based on inputs.enable_thinking
    if (string_ends_with(data.prompt, "<|inner_prefix|>")) {
        if (!inputs.enable_thinking) {
            data.prompt += "<|inner_suffix|>";
        } else {
            data.thinking_forced_open = true;
        }
    }

    data.preserved_tokens = {
        "<|system_start|>",
        "<|system_end|>",
        "<|developer_start|>",
        "<|developer_end|>",
        "<|user_start|>",
        "<|user_end|>",
        "<|assistant_start|>",
        "<|assistant_end|>",
        "<|inner_prefix|>",
        "<|inner_suffix|>",
        "<|tools_prefix|>",
        "<|tools_suffix|>",
    };

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;
    auto include_grammar = true;

    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;
        auto reasoning = p.eps();
        if (inputs.enable_thinking && extract_reasoning) {
            auto reasoning_content = p.tag(Tag::REASONING, p.until("<|inner_suffix|>")) + ("<|inner_suffix|>" | p.end());
            if (data.thinking_forced_open) {
                reasoning = reasoning_content;
            } else {
                reasoning = p.optional("<|inner_prefix|>" + reasoning_content);
            }
        }

        // Response format parser
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            return reasoning << p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema));
        }

        // Tool call parser - short form JSON array format
        // Format: <|tools_prefix|>[{"func_name": {...}}]<|tools_suffix|>
        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            // Tool call: <|tools_prefix|> + JSON array + <|tools_suffix|>
            auto tool_call = p.tag(Tag::TOOL,
                p.token_tag(Tag::TOOL_OPEN, "<|tools_prefix|>")
                << p.tag(Tag::TOOL_ARGS, p.until("<|tools_suffix|>"))
                << p.token_tag(Tag::TOOL_CLOSE, "<|tools_suffix|>")
            );

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_calls = p.trigger_rule("tool-call", p.repeat(tool_call, min_calls, max_calls));

            return reasoning << p.tag(Tag::CONTENT, p.until("<|tools_prefix|>")) << tool_calls;
        }

        // Content only parser
        include_grammar = false;
        return reasoning << p.tag(Tag::CONTENT, p.rest());
    });

    data.parser = parser.save();

    if (include_grammar) {
        data.grammar_lazy = has_tools && inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_AUTO;

        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            auto schemas = json::array();
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                // Apertus uses short form: {"func_name": {"arg1": value1}}
                schemas.push_back({
                    {"type", "object"},
                    {"properties", {
                        {function.at("name"), function.at("parameters")}
                    }},
                    {"required", json::array({function.at("name")})},
                });
            });
            auto schema = json{
                {"type", "array"},
                {"items", schemas.size() == 1 ? schemas[0] : json{{"anyOf", schemas}}},
                {"minItems", 1},
            };
            if (!inputs.parallel_tool_calls) {
                schema["maxItems"] = 1;
            }
            builder.add_rule("root",
                std::string(data.thinking_forced_open ? "( \"<|inner_suffix|>\" space )? " : "") +
                "\"<|tools_prefix|>\" space " + builder.add_schema("tool_calls", schema) + " space \"<|tools_suffix|>\"");
        });

        data.grammar_triggers = {{COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL,
            // If thinking_forced_open, then we capture the <|inner_suffix|> tag in the grammar
            std::string(data.thinking_forced_open ?
                "[\\s\\S]*?(<\\|inner_suffix\\|>\\s*)" :
                "(?:<\\|inner_prefix\\|>[\\s\\S]*?<\\|inner_suffix\\|>\\s*)?") +
            "(<\\|tools_prefix\\|>)[\\s\\S]*"}};
    }

    return data;
}
 128 changes: 128 additions & 0 deletions128  
common/chat-parsers/apriel-1-5.cpp
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,128 @@
// Apriel 1.5 tool call format
// Format: <tool_calls>[{"name": "func", "arguments": {...}}]</tool_calls>
// With optional <thinking>...</thinking> reasoning blocks

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_apriel_1_5(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_APRIEL_1_5;

    // Handle thinking tags appropriately based on inputs.enable_thinking
    if (string_ends_with(data.prompt, "<thinking>\n") || string_ends_with(data.prompt, "<thinking>")) {
        if (!inputs.enable_thinking) {
            data.prompt += "</thinking>";
        } else {
            data.thinking_forced_open = true;
        }
    }

    data.preserved_tokens = {
        "<thinking>",
        "</thinking>",
        "<tool_calls>",
        "</tool_calls>",
    };

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;
    auto include_grammar = true;

    const bool require_tools = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED;
    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;
        const bool has_reasoning = inputs.enable_thinking && extract_reasoning;

        auto reasoning_block = p.eps();
        if (has_reasoning) {
            auto reasoning_content = p.tag(Tag::REASONING, p.until("</thinking>")) + ("</thinking>" | p.end());
            reasoning_block = data.thinking_forced_open
                ? reasoning_content
                : p.token("<thinking>") + reasoning_content;
        }

        auto build_content_expr = [&](const std::string & delimiter) {
            auto base_content = p.tag(Tag::CONTENT, p.until(delimiter));
            if (!has_reasoning) {
                return base_content;
            }

            auto content_before_reasoning = p.tag(Tag::CONTENT, p.until("<thinking>"));
            auto content_after_reasoning = p.tag(Tag::CONTENT, p.until(delimiter));
            auto reasoning_after_content = p.atomic(content_before_reasoning + reasoning_block + content_after_reasoning);
            auto reasoning_only = p.atomic(reasoning_block + content_after_reasoning);
            return p.choice({reasoning_after_content, reasoning_only, base_content});
        };

        auto parse_content_until = [&](const std::string & marker) {
            return p.choice({build_content_expr("\n" + marker), build_content_expr(marker)});
        };

        auto consume_end = [&]() {
            return p.optional(p.literal("\n"))
                + p.optional(p.literal("<|end|>"))
                + p.optional(p.literal("\n"));
        };

        // Response format parser
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            return (has_reasoning ? p.optional(reasoning_block) : p.eps())
                << p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema))
                << consume_end();
        }

        // Tool call parser
        // Format: <tool_calls>[{"name": "func", "arguments": {...}}]</tool_calls>
        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            auto tool_call = p.tag(Tag::TOOL,
                p.token_tag(Tag::TOOL_OPEN, "<tool_calls>")
                + p.tag(Tag::TOOL_ARGS, p.until("</tool_calls>"))
                + p.token_tag(Tag::TOOL_CLOSE, "</tool_calls>")
            );

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_calls = p.trigger_rule("tool-call", p.repeat(tool_call, min_calls, max_calls));
            auto newline_before_tools = p.optional(p.literal("\n"));

            if (require_tools) {
                return (has_reasoning ? p.optional(reasoning_block) : p.eps())
                    << newline_before_tools
                    << tool_calls
                    << consume_end();
            }

            auto content_before_tools = parse_content_until("<tool_calls>");
            return content_before_tools << newline_before_tools << tool_calls << consume_end();
        }

        // Content only parser
        include_grammar = false;
        return parse_content_until("<|end|>") << consume_end();
    });

    data.parser = parser.save();

    if (include_grammar) {
        data.grammar_lazy = has_tools && inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_AUTO;

        // Build grammar from PEG parser
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            foreach_function(inputs.tools, [&](const json & tool) {
                auto schema = tool.at("function").at("parameters");
                builder.resolve_refs(schema);
            });
            parser.build_grammar(builder, data.grammar_lazy);
        });

        if (data.grammar_lazy) {
            data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<tool_calls>"});
        } else {
            data.grammar_triggers.clear();
        }
    }

    return data;
}
 152 changes: 152 additions & 0 deletions152  
common/chat-parsers/command-r7b.cpp
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,152 @@
// Command R7B tool call format
// Format: <|START_THINKING|>...<|END_THINKING|><|START_ACTION|>[{"tool_call_id":"1","tool_name":"func","parameters":{}}]<|END_ACTION|>

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_command_r7b(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    auto adjusted_messages = json::array();
    for (const auto & msg : inputs.messages) {
        auto has_reasoning_content = msg.contains("reasoning_content") && msg.at("reasoning_content").is_string();
        auto has_tool_calls = msg.contains("tool_calls") && msg.at("tool_calls").is_array();
        if (has_reasoning_content && has_tool_calls) {
            auto adjusted_message = msg;
            adjusted_message["tool_plan"] = msg.at("reasoning_content");
            adjusted_message.erase("reasoning_content");
            adjusted_messages.push_back(adjusted_message);
        } else {
            adjusted_messages.push_back(msg);
        }
    }
    data.prompt = apply(tmpl, inputs, /* messages_override= */ adjusted_messages);

    if (string_ends_with(data.prompt, "<|START_THINKING|>")) {
        if (!inputs.enable_thinking) {
            data.prompt += "<|END_THINKING|>";
        } else {
            data.thinking_forced_open = true;
        }
    } else if (!inputs.enable_thinking && string_ends_with(data.prompt, "<|CHATBOT_TOKEN|>")) {
        data.prompt += "<|START_THINKING|><|END_THINKING|>";
    }

    bool has_tools = inputs.tools.is_array() && !inputs.tools.empty();

    data.format = COMMON_CHAT_FORMAT_COMMAND_R7B;
    data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;

    data.preserved_tokens = {
        "<|START_ACTION|>",
        "<|END_ACTION|>",
        "<|START_RESPONSE|>",
        "<|END_RESPONSE|>",
        "<|START_THINKING|>",
        "<|END_THINKING|>",
    };

    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;

    // Build PEG parser
    const bool require_tools = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED;
    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;

        auto response_block = p.optional(
            p.optional(p.literal("<|START_OF_TURN_TOKEN|>"))
            + p.optional(p.literal("<|CHATBOT_TOKEN|>"))
            + (p.literal("<|START_RESPONSE|>") | p.literal("RESPONSE|>"))
            + p.tag(Tag::CONTENT, p.until_one_of({"<|END_RESPONSE|>", "END_RESPONSE|>"}))
            + (p.literal("<|END_RESPONSE|>") | p.literal("END_RESPONSE|>"))
        );

        // Always handle thinking block (consume tags even if not extracting reasoning)
        auto reasoning = p.eps();
        if (data.thinking_forced_open) {
            // Thinking was already started by template
            if (extract_reasoning) {
                reasoning = p.tag(Tag::REASONING, p.until("<|END_THINKING|>")) + "<|END_THINKING|>";
            } else {
                reasoning = p.until("<|END_THINKING|>") + "<|END_THINKING|>";
            }
        } else {
            if (extract_reasoning) {
                reasoning = p.optional("<|START_THINKING|>" + p.tag(Tag::REASONING, p.until("<|END_THINKING|>")) + "<|END_THINKING|>");
            } else {
                reasoning = p.optional("<|START_THINKING|>" + p.until("<|END_THINKING|>") + "<|END_THINKING|>");
            }
        }

        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            // Tool call: <|START_ACTION|>[...json array...]<|END_ACTION|>
            auto tool_call = p.tag(Tag::TOOL,
                p.token_tag(Tag::TOOL_OPEN, "<|START_ACTION|>")
                + p.tag(Tag::TOOL_ARGS, p.json())  // JSON array with tool calls
                + p.token_tag(Tag::TOOL_CLOSE, "<|END_ACTION|>")
            );

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_calls = p.trigger_rule("tool-call", p.repeat(tool_call, min_calls, max_calls));

            if (require_tools) {
                return reasoning << response_block << tool_calls << p.optional(p.rest());
            }

            return reasoning << response_block << tool_calls << p.optional(p.rest());
        }

        // Content only parser
        return reasoning << response_block << p.optional(p.rest());
    });

    data.parser = parser.save();

    if (has_tools) {
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            auto schemas = json::array();
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                schemas.push_back({
                    {"type", "object"},
                    {"properties", {
                        {"tool_call_id", {
                            {"type", "string"},
                            // Command-R's template expects an integer string.
                            {"pattern", "^[0-9]{1,10}$"},
                        }},
                        {"tool_name", {
                            {"type", "string"},
                            {"const", function.at("name")},
                        }},
                        {"parameters", function.at("parameters")},
                    }},
                    {"required", json::array({"tool_call_id", "tool_name", "parameters"})},
                });
            });
            auto schema = json {
                {"type", "array"},
                {"items", schemas.size() == 1 ? schemas[0] : json {{"anyOf", schemas}}},
                {"minItems", 1},
            };
            if (!inputs.parallel_tool_calls) {
                schema["maxItems"] = 1;
            }
            builder.add_rule("root",
                std::string(data.thinking_forced_open ? "( \"<|END_THINKING|>\" space )? " : "") +
                "\"<|START_ACTION|>\" " + builder.add_schema("tool_calls", schema) + " \"<|END_ACTION|>\"");
        });

        if (data.grammar_lazy) {
            data.grammar_triggers.push_back({
                COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL,
                std::string(data.thinking_forced_open ? "[\\s\\S]*?(<\\|END_THINKING\\|>\\s*)" : "(?:<\\|START_THINKING\\|>[\\s\\S]*?<\\|END_THINKING\\|>\\s*)?") +
                    "(<\\|START_ACTION\\|>)[\\s\\S]*"
            });
        } else {
            data.grammar_triggers.clear();
        }
    }

    return data;
}
 168 changes: 168 additions & 0 deletions168  
common/chat-parsers/deepseek-r1.cpp
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,168 @@
// DeepSeek R1 tool call format
// Format: <｜tool▁calls▁begin｜><｜tool▁call▁begin｜>function<｜tool▁sep｜>name
// ```json
// {"arg":"value"}
// ```<｜tool▁call▁end｜><｜tool▁calls▁end｜>
// With optional <think>...</think> reasoning blocks

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_deepseek_r1(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;
    auto prompt = apply(tmpl, inputs);

    // Hacks to fix the official (broken) prompt.
    // It is advisable to use --chat-template-file models/templates/llama-cpp-deepseek-r1.jinja instead,
    // until the official template is fixed.
    if (tmpl.source().find("{% if ns.is_tool %}{{'<｜tool▁outputs▁end｜>'}}") != std::string::npos) {
        // Don't leave the chat dangling after tool results
        if (string_ends_with(prompt, "<｜tool▁outputs▁end｜>")) {
            prompt += "<｜end▁of▁sentence｜>";
            if (inputs.add_generation_prompt) {
                prompt += "<｜Assistant｜>";
            }
        }
        // Fix up tool call delta example added by Minja
        prompt = std::regex_replace(
            prompt,
            std::regex("(<｜tool▁call▁end｜>)[\\s\\r\\n]*(<｜tool▁outputs▁begin｜>|<｜User｜>)"),
            "$1<｜tool▁calls▁end｜><｜end▁of▁sentence｜>$2");
    }
    data.prompt = prompt;

    if (string_ends_with(data.prompt, "<think>\n")) {
        if (!inputs.enable_thinking) {
            data.prompt += "</think>";
        } else {
            data.thinking_forced_open = true;
        }
    }

    bool has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;

    data.format = COMMON_CHAT_FORMAT_DEEPSEEK_R1;
    data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED && inputs.json_schema.is_null();

    data.preserved_tokens = {
        "<think>",
        "</think>",
        "<｜tool▁calls▁begin｜>",
        "<｜tool▁call▁begin｜>",
        "<｜tool▁sep｜>",
        "<｜tool▁call▁end｜>",
        "<｜tool▁calls▁end｜>",
    };

    // Build PEG parser
    bool require_tools = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED;
    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;

        auto consume_eos = [&]() {
            return p.optional(p.literal("<｜end▁of▁sentence｜>")) + p.optional(p.space());
        };

        // Optional thinking block
        auto reasoning = p.eps();
        if (extract_reasoning) {
            if (data.thinking_forced_open) {
                reasoning = p.tag(Tag::REASONING, p.until("</think>")) + "</think>";
            } else {
                reasoning = p.optional("<think>" + p.tag(Tag::REASONING, p.until("</think>")) + "</think>");
            }
        }

        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            auto tool_choice = p.choice();

            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");

                // Format: function<｜tool▁sep｜>name\n```json\n{...}\n```<｜tool▁call▁end｜>
                tool_choice |= p.rule("tool-" + name, p.tag(Tag::TOOL,
                    p.optional(p.token_tag(Tag::TOOL_OPEN, "<｜tool▁call▁begin｜>"))
                    + "function" + p.token("<｜tool▁sep｜>") + p.literal_tag(Tag::TOOL_NAME, name) + "\n```json\n"
                    + p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "tool-" + name + "-args", parameters))
                    + "\n```" + p.token_tag(Tag::TOOL_CLOSE, "<｜tool▁call▁end｜>")
                ));
            });

            // Accept multiple variants of the tool calls begin marker
            auto tool_calls_begin = p.choice()
                | "<｜tool▁calls▁begin｜>"
                | "<｜tool_calls_begin｜>"
                | "<｜tool calls begin｜>"
                | "<｜tool\\_calls\\_begin｜>"
                | "<｜tool▁calls｜>";

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_calls = p.trigger_rule("tool-call",
                tool_calls_begin + p.repeat(tool_choice, min_calls, max_calls) + "<｜tool▁calls▁end｜>"
            ) << consume_eos();

            // Content until tool calls marker
            auto content = p.tag(Tag::CONTENT, p.until_one_of({
                "<｜tool▁calls▁begin｜>",
                "<｜tool_calls_begin｜>",
                "<｜tool calls begin｜>",
                "<｜tool\\_calls\\_begin｜>",
                "<｜tool▁calls｜>",
            }));

            if (require_tools) {
                return reasoning << tool_calls;
            }
            return reasoning << content << tool_calls;
        }

        // Content only parser
        auto content_only = p.sequence({
            p.tag(Tag::CONTENT, p.until("<｜end▁of▁sentence｜>")),
            consume_eos()
        });
        return reasoning << p.choice({content_only, p.tag(Tag::CONTENT, p.rest())});
    });

    data.parser = parser.save();

    if (has_tools) {
        // Build grammar manually for backward compatibility
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            std::vector<std::string> tool_rules;
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");
                builder.resolve_refs(parameters);
                tool_rules.push_back(builder.add_rule(name + "-call",
                    "( \"<｜tool▁call▁begin｜>\" )? \"function<｜tool▁sep｜>" + name + "\\n"
                    "```json\\n\" " + builder.add_schema(name + "-args", parameters) + " "
                    "\"\\n```<｜tool▁call▁end｜>\""));
            });
            // Distill Qwen 7B & 32B models seem confused re/ syntax of their tool call opening tag,
            // so we accept common variants (then it's all constrained)
            builder.add_rule("root",
                std::string(data.thinking_forced_open ? "( \"</think>\" space )? " : "") +
                "( \"<｜tool▁calls▁begin｜>\" | \"<｜tool_calls_begin｜>\" | \"<｜tool calls begin｜>\" | \"<｜tool\\\\_calls\\\\_begin｜>\" | \"<｜tool▁calls｜>\" ) "
                "(" + string_join(tool_rules, " | ") + ")" + (inputs.parallel_tool_calls ? "*" : "") + " "
                "\"<｜tool▁calls▁end｜>\""
                " space");
        });

        if (data.grammar_lazy) {
            data.grammar_triggers.push_back({
                COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL,
                std::string(data.thinking_forced_open ? "[\\s\\S]*?(</think>\\s*)" : "(?:<think>[\\s\\S]*?</think>\\s*)?") +
                    "(<｜tool▁calls▁begin｜>|<｜tool_calls_begin｜>|<｜tool calls begin｜>|<｜tool\\\\_calls\\\\_begin｜>|<｜tool▁calls｜>)[\\s\\S]*"
            });
        } else {
            data.grammar_triggers.clear();
        }
    }

    return data;
}
 156 changes: 156 additions & 0 deletions156  
common/chat-parsers/deepseek-v3-1.cpp
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,156 @@
// DeepSeek V3.1 tool call format
// Format: <｜tool▁calls▁begin｜><｜tool▁call▁begin｜>name<｜tool▁sep｜>{"arg":"value"}<｜tool▁call▁end｜><｜tool▁calls▁end｜>
// With optional <think>...</think> reasoning blocks

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_deepseek_v3_1(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    // Pass thinking context for DeepSeek V3.1 template
    json additional_context = {
        {"thinking", inputs.enable_thinking},
    };

    auto prompt = apply(tmpl, inputs,
                       /* messages_override= */ inputs.messages,
                       /* tools_override= */ std::nullopt,
                       additional_context);
    data.prompt = prompt;

    if (string_ends_with(data.prompt, "<think>")) {
        if (!inputs.enable_thinking) {
            data.prompt += "</think>";
        } else {
            data.thinking_forced_open = true;
        }
    }

    bool has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;

    data.format = COMMON_CHAT_FORMAT_DEEPSEEK_V3_1;
    data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED && inputs.json_schema.is_null();

    data.preserved_tokens = {
        "<think>",
        "</think>",
        "<｜tool▁calls▁begin｜>",
        "<｜tool▁call▁begin｜>",
        "<｜tool▁sep｜>",
        "<｜tool▁call▁end｜>",
        "<｜tool▁calls▁end｜>",
    };

    // Build PEG parser
    bool require_tools = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED;
    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;

        auto consume_eos = [&]() {
            return p.optional(p.literal("<｜end▁of▁sentence｜>")) + p.optional(p.space());
        };

        // Optional thinking block
        auto reasoning = p.eps();
        if (extract_reasoning) {
            if (data.thinking_forced_open) {
                reasoning = p.tag(Tag::REASONING, p.until("</think>")) + "</think>";
            } else {
                reasoning = p.optional("<think>" + p.tag(Tag::REASONING, p.until("</think>")) + "</think>");
            }
        }

        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            auto tool_choice = p.choice();

            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");

                // Format: name<｜tool▁sep｜>{...}<｜tool▁call▁end｜>
                tool_choice |= p.rule("tool-" + name, p.tag(Tag::TOOL,
                    p.optional(p.token_tag(Tag::TOOL_OPEN, "<｜tool▁call▁begin｜>"))
                    + p.literal_tag(Tag::TOOL_NAME, name) + p.token("<｜tool▁sep｜>")
                    + p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "tool-" + name + "-args", parameters))
                    + p.token_tag(Tag::TOOL_CLOSE, "<｜tool▁call▁end｜>")
                ));
            });

            // Accept multiple variants of the tool calls begin marker
            auto tool_calls_begin = p.choice()
                | "<｜tool▁calls▁begin｜>"
                | "<｜tool_calls_begin｜>"
                | "<｜tool calls begin｜>"
                | "<｜tool\\_calls\\_begin｜>"
                | "<｜tool▁calls｜>";

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_calls = p.trigger_rule("tool-call",
                tool_calls_begin + p.repeat(tool_choice, min_calls, max_calls) + "<｜tool▁calls▁end｜>"
            ) << consume_eos();

            // Content until tool calls marker
            auto content = p.tag(Tag::CONTENT, p.until_one_of({
                "<｜tool▁calls▁begin｜>",
                "<｜tool_calls_begin｜>",
                "<｜tool calls begin｜>",
                "<｜tool\\_calls\\_begin｜>",
                "<｜tool▁calls｜>",
            }));

            if (require_tools) {
                return reasoning << tool_calls;
            }
            return reasoning << content << tool_calls;
        }

        // Content only parser
        auto content_only = p.sequence({
            p.tag(Tag::CONTENT, p.until("<｜end▁of▁sentence｜>")),
            consume_eos()
        });
        return reasoning << p.choice({content_only, p.tag(Tag::CONTENT, p.rest())});
    });

    data.parser = parser.save();

    if (has_tools) {
        // Build grammar manually for backward compatibility
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            std::vector<std::string> tool_rules;
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");
                builder.resolve_refs(parameters);
                tool_rules.push_back(builder.add_rule(name + "-call",
                    "( \"<｜tool▁call▁begin｜>\" )? \"" + name + "<｜tool▁sep｜>"
                    "\" " + builder.add_schema(name + "-args", parameters) + " "
                    "\"<｜tool▁call▁end｜>\""));
            });
            // Distill Qwen 7B & 32B models seem confused re/ syntax of their tool call opening tag,
            // so we accept common variants (then it's all constrained)
            builder.add_rule("root",
                std::string(data.thinking_forced_open ? "( \"</think>\" space )? " : "") +
                "( \"<｜tool▁calls▁begin｜>\" | \"<｜tool_calls_begin｜>\" | \"<｜tool calls begin｜>\" | \"<｜tool\\\\_calls\\\\_begin｜>\" | \"<｜tool▁calls｜>\" ) "
                "(" + string_join(tool_rules, " | ") + ")" + (inputs.parallel_tool_calls ? "*" : "") + " "
                "\"<｜tool▁calls▁end｜>\""
                " space");
        });

        if (data.grammar_lazy) {
            data.grammar_triggers.push_back({
                COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL,
                std::string(data.thinking_forced_open ? "[\\s\\S]*?(</think>\\s*)" : "(?:<think>[\\s\\S]*?</think>\\s*)?") +
                    "(<｜tool▁calls▁begin｜>|<｜tool_calls_begin｜>|<｜tool calls begin｜>|<｜tool\\\\_calls\\\\_begin｜>|<｜tool▁calls｜>)[\\s\\S]*"
            });
        } else {
            data.grammar_triggers.clear();
        }
    }

    return data;
}
 89 changes: 89 additions & 0 deletions89  
common/chat-parsers/firefunction-v2.cpp
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,89 @@
// Firefunction V2 tool call format
// Format: functools[{"name":"func","arguments":{}}]

#include "chat-parsers-internal.h"
common_chat_params common_chat_params_init_firefunction_v2(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    const std::optional<json> tools_override = json();
    const std::optional<json> additional_context = json {
        {"datetime", format_time(inputs.now, "%b %d %Y %H:%M:%S GMT")},
        {"functions", json(inputs.tools.empty() ? "" : inputs.tools.dump(2))},
    };
    data.prompt = apply(tmpl, inputs, /* messages_override =*/ std::nullopt, tools_override, additional_context);

    bool has_tools = inputs.tools.is_array() && !inputs.tools.empty();

    if (has_tools) {
        data.format = COMMON_CHAT_FORMAT_FIREFUNCTION_V2;
        data.preserved_tokens = {
            " functools[",
        };

        // Build the PEG parser
        bool require_tools = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED;
        auto parser = build_chat_peg_parser([&](auto & p) {
            using Tag = common_chat_peg_tag;

            if (inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
                // Tool call parser: content followed by functools[ and JSON array
                auto tool_call = p.tag(Tag::TOOL,
                    p.token_tag(Tag::TOOL_OPEN, " functools")
                    + p.tag(Tag::TOOL_ARGS, p.json())
                );

                auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
                auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
                auto tool_calls = p.trigger_rule("tool-call", p.repeat(tool_call, min_calls, max_calls));

                if (require_tools) {
                    return tool_calls;
                }
                return p.tag(Tag::CONTENT, p.until(" functools")) + tool_calls;
            }

            // Content only parser
            return p.tag(Tag::CONTENT, p.rest());
        });

        data.parser = parser.save();

        data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            auto schemas = json::array();
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                schemas.push_back({
                    {"type", "object"},
                    {"properties", {
                        {"name", {
                            {"type", "string"},
                            {"const", function.at("name")},
                        }},
                        {"arguments", function.at("parameters")},
                    }},
                    {"required", json::array({"name", "arguments", "id"})},
                });
            });
            auto schema = json {
                {"type", "array"},
                {"items", schemas.size() == 1 ? schemas[0] : json {{"anyOf", schemas}}},
                {"minItems", 1},
            };
            if (!inputs.parallel_tool_calls) {
                schema["maxItems"] = 1;
            }
            builder.add_rule("root", "\" functools\"? " + builder.add_schema("tool_calls", schema));
        });

        if (data.grammar_lazy) {
            data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, " functools["});
        } else {
            data.grammar_triggers.clear();
        }
    } else {
        data.format = COMMON_CHAT_FORMAT_CONTENT_ONLY;
    }

    return data;
}
 177 changes: 177 additions & 0 deletions177  
common/chat-parsers/function-gemma.cpp
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,177 @@
// FunctionGemma tool call format
// Format: <start_function_call>call:name{key:<escape>value<escape>,key2:123}<end_function_call>
// String values are wrapped with <escape> tokens, non-string values are raw.

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_function_gemma(const common_chat_template & tmpl, const struct templates_params & params) {
    common_chat_params data;
    data.grammar_lazy = params.tools.is_array() && !params.tools.empty() && params.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;

    data.prompt = apply(tmpl, params);
    data.format = COMMON_CHAT_FORMAT_FUNCTION_GEMMA;

    data.preserved_tokens = {
        "<start_function_call>",
        "<end_function_call>",
        "<start_function_response>",
        "<end_function_response>",
        "<escape>",
        "<end_of_turn>",
    };

    data.additional_stops.push_back("<end_function_call>");

    bool has_tools = params.tools.is_array() && !params.tools.empty();

    // Build the PEG parser for FunctionGemma format
    // Format: <start_function_call>call:name{key:<escape>value<escape>,key2:123}<end_function_call>
    bool require_tools = params.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED;
    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;

        // Token-aware parsers for FunctionGemma special tokens
        auto escape = p.token("<escape>");
        auto start_function_call = p.token("<start_function_call>");
        auto end_function_call = p.token("<end_function_call>");

        // Identifier pattern: [a-zA-Z_][a-zA-Z0-9_]*
        auto identifier = p.chars("a-zA-Z_", 1, 1) + p.chars("a-zA-Z0-9_", 0, -1);

        // Argument name: alphanumeric identifier before ':'
        auto arg_name = p.atomic_tag(Tag::TOOL_ARG_NAME, identifier);

        // String value: <escape>...<escape> with content captured
        // Token-aware matching ensures we don't match partial token sequences
        auto string_value = escape + p.tag(Tag::TOOL_ARG_STRING_VALUE, p.until_token("<escape>")) + escape;

        // JSON value: raw number, boolean, null, array, or object (without escape delimiters)
        auto json_value = p.tag(Tag::TOOL_ARG_JSON_VALUE, p.json());

        // An argument is: name:(string_value | json_value)
        auto arg = p.tag(Tag::TOOL_ARG, arg_name + ":" + (string_value | json_value));

        // Arguments list: {arg1,arg2,...} or {}
        auto args = "{" + p.optional(arg + p.zero_or_more("," + arg)) + "}";

        // Tool name: alphanumeric identifier after "call:"
        auto tool_name = p.atomic_tag(Tag::TOOL_NAME, identifier);

        auto end_of_turn = p.optional(p.literal("<end_of_turn>"));

        // Tool call: <start_function_call>call:name{...}<end_function_call>
        auto tool_call = p.tag(Tag::TOOL,
            p.atomic_tag(Tag::TOOL_OPEN, start_function_call + "call:")
            + tool_name
            + args
            + p.atomic_tag(Tag::TOOL_CLOSE, end_function_call)
        );

        // Content before tool calls (token-aware matching)
        auto content = p.tag(Tag::CONTENT, p.until_token("<start_function_call>"));

        if (has_tools && params.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            int min_calls = params.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            int max_calls = params.parallel_tool_calls ? -1 : 1;
            auto calls = p.repeat(tool_call, min_calls, max_calls);
            if (require_tools) {
                return calls + end_of_turn;
            }
            return content + calls + end_of_turn;
        }

        // Content only
        auto content_only = p.choice({
            p.tag(Tag::CONTENT, p.until_token("<end_of_turn>")) + end_of_turn,
            p.tag(Tag::CONTENT, p.rest())
        });
        return content_only;
    });

    data.parser = parser.save();

    if (has_tools) {
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            std::vector<std::string> tool_rules;

            foreach_function(params.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                const auto & parameters = function.at("parameters");

                // Build parameter rules for this function
                std::vector<std::string> param_rules;
                if (parameters.contains("properties")) {
                    const auto & props = parameters.at("properties");
                    std::set<std::string> required_set;
                    if (parameters.contains("required")) {
                        for (const auto & r : parameters.at("required")) {
                            required_set.insert(r.get<std::string>());
                        }
                    }

                    for (auto it = props.begin(); it != props.end(); ++it) {
                        std::string param_name = it.key();
                        const auto & prop = it.value();

                        // Determine if this is a string type
                        bool is_string = prop.contains("type") && prop.at("type") == "string";
                        bool is_required = required_set.count(param_name) > 0;

                        std::string value_rule;
                        if (is_string) {
                            // String values use <escape>...</escape> delimiters
                            // Content inside can be any chars except <escape>
                            value_rule = "\"<escape>\" [^<]* \"<escape>\"";
                        } else {
                            // Non-string values are raw (numbers, booleans, etc.)
                            // Use JSON value rule for flexibility
                            value_rule = builder.add_schema(name + "_" + param_name + "_value", prop);
                        }

                        std::string param_rule = "\"" + param_name + ":\" " + value_rule;
                        if (!is_required) {
                            param_rule = "( " + param_rule + " )?";
                        }
                        param_rules.push_back(param_rule);
                    }
                }

                // Build function rule: call:name{param1:val1,param2:val2}
                std::string params_content;
                if (param_rules.empty()) {
                    params_content = "";
                } else {
                    // Join parameters with comma
                    params_content = param_rules[0];
                    for (size_t i = 1; i < param_rules.size(); ++i) {
                        params_content += " \",\" " + param_rules[i];
                    }
                }

                std::string fn_rule = "\"call:" + name + "{\" " + params_content + " \"}\"";
                std::string rule_name = builder.add_rule(name + "_call", fn_rule);
                tool_rules.push_back(rule_name);
            });

            // Root rule: <start_function_call>...tool_call...<end_function_call>
            std::string tool_call_alt = tool_rules.size() == 1 ? tool_rules[0] : "( " + string_join(tool_rules, " | ") + " )";
            std::string root_rule = "\"<start_function_call>\" " + tool_call_alt + " \"<end_function_call>\"";

            if (params.parallel_tool_calls) {
                // Allow multiple tool calls
                builder.add_rule("root", "( " + root_rule + " )+");
            } else {
                builder.add_rule("root", root_rule);
            }
        });

        if (data.grammar_lazy) {
            data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<start_function_call>"});
        } else {
            data.grammar_triggers.clear();
        }
    }

    return data;
}
 135 changes: 135 additions & 0 deletions135  
common/chat-parsers/functionary-v3-1-llama-3-1.cpp
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,135 @@
// Functionary v3.1 (Llama 3.1 style) tool call format
// Format: <function=name>{...}</function>
// Also supports: <|python_tag|>code...

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_functionary_v3_1_llama_3_1(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    std::string python_code_argument_name;
    auto has_raw_python = false;
    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();

    if (has_tools) {
        data.format = COMMON_CHAT_FORMAT_FUNCTIONARY_V3_1_LLAMA_3_1;
        data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;

        // Detect python tool with string argument
        foreach_function(inputs.tools, [&](const json & tool) {
            const auto & function = tool.at("function");
            const auto & parameters = function.at("parameters");
            std::string name = function.at("name");
            if (name == "python" || name == "ipython") {
                if (!parameters.contains("type")) {
                    throw std::runtime_error("Missing type in python tool");
                }
                has_raw_python = true;
                const auto & type = parameters.at("type");
                if (type == "object") {
                    auto properties = parameters.at("properties");
                    for (auto it = properties.begin(); it != properties.end(); ++it) {
                        if (it.value().at("type") == "string") {
                            if (!python_code_argument_name.empty()) {
                                throw std::runtime_error("Multiple string arguments found in python tool");
                            }
                            python_code_argument_name = it.key();
                        }
                    }
                    if (python_code_argument_name.empty()) {
                        throw std::runtime_error("No string argument found in python tool");
                    }
                } else if (type != "string") {
                    throw std::runtime_error("Invalid type in python tool: " + type.dump());
                }
            }
        });

        // Set up preserved tokens
        data.preserved_tokens = {};
        if (has_raw_python) {
            data.preserved_tokens.push_back("<|python_tag|>");
        }

        // Build PEG parser for <function=name>{...}</function> format
        auto parser = build_chat_peg_parser([&](auto & p) {
            using Tag = common_chat_peg_tag;

            // Response format parser
            if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
                return p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema));
            }

            // Tool call parser
            if (inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
                auto tool_choice = p.choice();

                foreach_function(inputs.tools, [&](const json & tool) {
                    const auto & function = tool.at("function");
                    std::string name = function.at("name");
                    auto parameters = function.at("parameters");

                    // Format: <function=name>{...}</function>
                    tool_choice |= p.rule("tool-" + name, p.tag(Tag::TOOL,
                        p.token_tag(Tag::TOOL_OPEN, "<function=")
                        + p.literal_tag(Tag::TOOL_NAME, name)
                        + ">"
                        + p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "tool-" + name + "-params", parameters))
                        + p.token_tag(Tag::TOOL_CLOSE, "</function>")
                    ));
                });

                // Add python tag support if present
                if (has_raw_python) {
                    // <|python_tag|>code... (raw python code wrapped in arguments)
                    tool_choice |= p.rule("python-raw", p.tag(Tag::TOOL,
                        p.atomic_tag(Tag::TOOL_OPEN, p.token("<|python_tag|>") + p.literal_tag(Tag::TOOL_NAME, "python"))
                        + p.tag(Tag::TOOL_ARGS, p.rest())
                    ));
                }

                auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
                auto max_calls = inputs.parallel_tool_calls ? -1 : 1;

                std::vector<std::string> delimiters = {"<function="};
                if (has_raw_python) {
                    delimiters.push_back("<|python_tag|>");
                }

                auto tool_calls = p.trigger_rule("tool-call", p.repeat(tool_choice, min_calls, max_calls));
                return p.tag(Tag::CONTENT, p.until_one_of(delimiters)) << tool_calls;
            }

            // Content only parser
            return p.tag(Tag::CONTENT, p.rest());
        });

        data.parser = parser.save();

        // Build grammar
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            std::vector<std::string> tool_rules;
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                tool_rules.push_back(builder.add_rule(name + "-call",
                    "\"<function=" + name + ">\" " +
                    builder.add_schema(name + "-args", function.at("parameters")) +
                    " \"</function>\" space"
                ));
            });
            if (has_raw_python) {
                tool_rules.push_back(builder.add_rule("python-call", "\"<|python_tag|>\" .*"));
                data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<|python_tag|>"});
            }
            auto tool_call = builder.add_rule("tool_call", string_join(tool_rules, " | ")) + " space";
            builder.add_rule("root", inputs.parallel_tool_calls ? "(" + tool_call + ")+" : tool_call);
            data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<function="});
        });
    } else {
        data.format = COMMON_CHAT_FORMAT_CONTENT_ONLY;
    }

    data.prompt = apply(tmpl, inputs);
    return data;
}
 148 changes: 148 additions & 0 deletions148  
common/chat-parsers/functionary-v3-2.cpp
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,148 @@
// Functionary v3.2 tool call format
// Format: >>>all\ntext>>>fn1\n{...}>>>fn2\n{...}...
// First tool call without >>>, subsequent with >>>
// Python tool can have raw code (without opening {)

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_functionary_v3_2(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;
    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_FUNCTIONARY_V3_2;

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();

    if (has_tools) {
        data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;

        // Build PEG parser for >>>function_name\n{...} format
        auto parser = build_chat_peg_parser([&](auto & p) {
            using Tag = common_chat_peg_tag;

            // Response format parser
            if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
                return p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema));
            }

            // Tool call parser
            if (inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
                // First tool call (without >>>)
                auto first_tool_choice = p.choice();

                foreach_function(inputs.tools, [&](const json & tool) {
                    const auto & function = tool.at("function");
                    std::string name = function.at("name");
                    auto parameters = function.at("parameters");

                    if (name == "python") {
                        // Python can have raw code or JSON
                        first_tool_choice |= p.rule("first-tool-" + name, p.tag(Tag::TOOL,
                            p.tag(Tag::TOOL_OPEN, p.eps()) + p.literal_tag(Tag::TOOL_NAME, name) + "\n"
                            + (p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "tool-" + name + "-params", parameters))
                               | p.tag(Tag::TOOL_ARGS, p.until(">>>")))
                        ));
                    } else {
                        // Regular JSON tool
                        first_tool_choice |= p.rule("first-tool-" + name, p.tag(Tag::TOOL,
                            p.tag(Tag::TOOL_OPEN, p.eps()) + p.literal_tag(Tag::TOOL_NAME, name) + "\n"
                            + p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "tool-" + name + "-params", parameters))
                        ));
                    }
                });

                // Subsequent tool calls (with >>>)
                auto subsequent_tool_choice = p.choice();

                foreach_function(inputs.tools, [&](const json & tool) {
                    const auto & function = tool.at("function");
                    std::string name = function.at("name");
                    auto parameters = function.at("parameters");

                    if (name == "python") {
                        subsequent_tool_choice |= p.rule("tool-" + name, p.tag(Tag::TOOL,
                            p.literal_tag(Tag::TOOL_OPEN, ">>>") + p.literal_tag(Tag::TOOL_NAME, name) + "\n"
                            + (p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "tool-" + name + "-params", parameters))
                               | p.tag(Tag::TOOL_ARGS, p.until(">>>")))
                        ));
                    } else {
                        subsequent_tool_choice |= p.rule("tool-" + name, p.tag(Tag::TOOL,
                            p.literal_tag(Tag::TOOL_OPEN, ">>>") + p.literal_tag(Tag::TOOL_NAME, name) + "\n"
                            + p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "tool-" + name + "-params", parameters))
                        ));
                    }
                });

                // Build pattern: first call or content, then subsequent tool calls
                // Format: name\n{...}  or  all\n<content>  or  all\n<content>>>>name\n{...}
                auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;

                // Content marker: "all\n" followed by text until >>> or end
                auto content_marker = "all\n" + p.tag(Tag::CONTENT, p.until(">>>"));

                // First element is either content or tool call
                auto first_element = content_marker | p.repeat(first_tool_choice, min_calls, 1);

                if (inputs.parallel_tool_calls) {
                    // Subsequent tool calls with >>> prefix
                    auto subsequent_calls = p.repeat(subsequent_tool_choice, 0, -1);
                    return p.trigger_rule("first-element", first_element) << subsequent_calls << p.tag(Tag::CONTENT, p.rest());
                } else {
                    // Just the first element
                    return p.trigger_rule("first-element", first_element) << p.tag(Tag::CONTENT, p.rest());
                }
            }

            // Content only parser
            return p.tag(Tag::CONTENT, p.rest());
        });

        data.parser = parser.save();

        // Build grammar
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            std::vector<std::string> first_tool_rules;
            std::vector<std::string> subsequent_tool_rules;

            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");
                builder.resolve_refs(parameters);

                std::string args_pattern = "[\\s\\S]*";
                auto args_rule = builder.add_schema(name + "-args", parameters);
                if (name == "python") {
                    args_rule = builder.add_rule(name + "-maybe-raw-args", args_rule + " | [^{] .*");
                } else {
                    args_pattern = "\\{" + args_pattern;
                }

                auto call_rule = builder.add_rule(name + "-call", "\"" + name + "\\n\" " + args_rule);
                first_tool_rules.push_back(call_rule);

                if (inputs.parallel_tool_calls) {
                    subsequent_tool_rules.push_back(builder.add_rule(name + "-call2", "\">>>\" " + call_rule));
                }

                data.grammar_triggers.push_back({
                    COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL,
                    "((?:[\\s\\S]+?>>>)?" + regex_escape(name) + "\n)" + args_pattern,
                });
            });

            data.preserved_tokens = {
                "<|end_header_id|>",
            };

            auto first_rule = first_tool_rules.empty() ? "" : builder.add_rule("first_tool_call", string_join(first_tool_rules, " | ")) + " space";
            if (inputs.parallel_tool_calls) {
                auto subsequent_rule = builder.add_rule("subsequent_tool_call", string_join(subsequent_tool_rules, " | ")) + " space";
                builder.add_rule("root", first_rule + " (" + subsequent_rule + ")*");
            } else {
                builder.add_rule("root", first_rule);
            }
        });
    }

    return data;
}
 112 changes: 112 additions & 0 deletions112  
common/chat-parsers/generic.cpp
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,112 @@
// Generic tool call format (fallback)
// Format: JSON with tool_call/tool_calls or response field
// Single: {"tool_call": {"name": "func", "arguments": {...}}}
// Multiple: {"tool_calls": [{"name": "func", "arguments": {...}}]}
// Response: {"response": "..."}

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_generic(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    auto tool_call_schemas = json::array();
    foreach_function(inputs.tools, [&](const json & tool) {
        const auto & function = tool.at("function");
        auto tool_schema = json {
            {"type", "object"},
            {"properties", {
                {"name", {
                    {"type", "string"},
                    {"const", function.at("name")},
                }},
                {"arguments", function.at("parameters")},
            }},
            {"required", json::array({"name", "arguments"})},
        };
        if (function.contains("description")) {
            tool_schema["description"] = function.at("description");
        }
        if (inputs.parallel_tool_calls) {
            tool_schema.at("properties")["id"] = {
                {"type", "string"},
                {"minLength", 4},
            };
            tool_schema.at("required").push_back("id");
        }
        tool_call_schemas.emplace_back(tool_schema);
    });
    const auto tool_call =
        inputs.parallel_tool_calls
            ? json {
                {"type", "object"},
                {"properties", {
                    {"tool_calls", {
                        {"type", "array"},
                        {"items", tool_call_schemas.size() == 1 ? tool_call_schemas[0] : json {
                            {"anyOf", tool_call_schemas},
                        }},
                        {"minItems", 1},
                    }},
                }},
                {"required", json::array({"tool_calls"})},
            }
            : json {
                {"type", "object"},
                {"properties", {
                    {"tool_call", tool_call_schemas.size() == 1 ? tool_call_schemas[0] : json {
                        {"anyOf", tool_call_schemas},
                    }},
                }},
                {"required", json::array({"tool_call"})},
            };
    const auto schema =
        inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED
            ? json {
                {"anyOf", json::array({
                    tool_call,
                    {
                        {"type", "object"},
                        {"properties", {
                            {"response", inputs.json_schema.is_null()
                                ? json {{"type", "string"}}
                                : inputs.json_schema
                            },
                        }},
                        {"required", json::array({"response"})},
                    },
                })}
            }
            : tool_call;

    data.grammar_lazy = false;
    data.grammar = build_grammar([&](const common_grammar_builder & builder) {
        builder.add_schema("root", schema);
    });

    // Build PEG parser for generic JSON format
    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();

    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;

        // The generic format uses JSON with specific structure
        // {"tool_call": {...}} or {"tool_calls": [...]} or {"response": "..."}
        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            // Parse as JSON and extract tool calls
            return p.tag(Tag::TOOL_ARGS, p.json());
        }

        // Content only - parse as JSON and extract response
        return p.tag(Tag::CONTENT, p.json());
    });

    data.parser = parser.save();

    auto tweaked_messages = common_chat_template::add_system(
        inputs.messages,
        "Respond in JSON format, either with `tool_call` (a request to call tools) or with `response` reply to the user's request");

    data.prompt = apply(tmpl, inputs, /* messages_override= */ tweaked_messages);
    data.format = COMMON_CHAT_FORMAT_GENERIC;
    return data;
}
 230 changes: 230 additions & 0 deletions230  
common/chat-parsers/glm-4-5.cpp
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,230 @@
// GLM 4.5 tool call format
// Format: <tool_call>function_name<arg_key>key</arg_key><arg_value>value</arg_value></tool_call>
// With optional <think>...</think> reasoning blocks

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_glm_4_5(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    std::string prompt = apply(tmpl, inputs);

    // match the existing trimming behavior
    if (inputs.add_bos && string_starts_with(prompt, tmpl.bos_token())) {
        prompt.erase(0, tmpl.bos_token().size());
    }
    if (inputs.add_eos && string_ends_with(prompt, tmpl.eos_token())) {
        prompt.erase(prompt.size() - tmpl.eos_token().size());
    }
    if (string_ends_with(prompt, "<think>")) {
        if (!inputs.enable_thinking) {
            prompt += "</think>";
        } else {
            data.thinking_forced_open = true;
        }
    }

    data.prompt = prompt;
    data.format = COMMON_CHAT_FORMAT_GLM_4_5;

    // add GLM preserved tokens
    data.preserved_tokens = {
        "<|endoftext|>",
        "[MASK]",
        "[gMASK]",
        "[sMASK]",
        "<sop>",
        "<eop>",
        "<|system|>",
        "<|user|>",
        "<|assistant|>",
        "<|observation|>",
        "<|begin_of_image|>",
        "<|end_of_image|>",
        "<|begin_of_video|>",
        "<|end_of_video|>",
        "<|begin_of_audio|>",
        "<|end_of_audio|>",
        "<|begin_of_transcription|>",
        "<|end_of_transcription|>",
        "<|code_prefix|>",
        "<|code_middle|>",
        "<|code_suffix|>",
        "/nothink",
        "<think>",
        "</think>",
        "<tool_call>",
        "</tool_call>",
        "<arg_key>",
        "</arg_key>",
        "<arg_value>",
        "</arg_value>"
    };

    // extra GLM 4.5 stop word
    data.additional_stops.insert(data.additional_stops.end(), {
        "<|user|>",
        "<|observation|>"
    });

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;
    auto include_grammar = true;

    const bool require_tools = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED;
    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;

        // Thinking block parser - extracts content from <think>...</think> into REASONING
        auto thinking_block = p.optional(p.literal("\n")) + "<think>" + p.tag(Tag::REASONING, p.until("</think>")) + "</think>";

        // When thinking_forced_open is true, we expect reasoning content without the opening <think>
        auto forced_thinking = p.optional(p.literal("\n")) + p.tag(Tag::REASONING, p.until("</think>")) + ("</think>" | p.end());

        // Response format parser
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            if (data.thinking_forced_open) {
                return forced_thinking + p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema));
            }
            return p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema));
        }

        // Tool call parser
        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            auto tool_choice = p.choice();
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");

                auto schema_info = common_schema_info();
                schema_info.resolve_refs(parameters);

                bool allow_additional = false;
                bool additional_has_schema = false;
                json additional_schema;
                if (parameters.contains("additionalProperties")) {
                    const auto & additional = parameters.at("additionalProperties");
                    if (additional.is_boolean()) {
                        allow_additional = additional.get<bool>();
                    } else if (additional.is_object()) {
                        allow_additional = true;
                        additional_has_schema = true;
                        additional_schema = additional;
                    }
                }

                // Format: <tool_call>name<arg_key>key</arg_key><arg_value>value</arg_value></tool_call>
                // Optional leading newline to handle both start-of-output and mid-content cases
                auto tool_open = p.optional(p.literal("\n")) + "<tool_call>" + p.literal_tag(Tag::TOOL_NAME, name) + "\n";
                // Tool close: just </tool_call>, optional newline consumed by content_after
                auto tool_close = p.literal("</tool_call>");
                auto args = p.sequence();
                auto arg_string = p.rule("xml-arg-string", p.until_one_of({
                    "</arg_value>",
                    "<arg_key>",
                    "</tool_call>"
                }));

                foreach_parameter(function, [&](const auto & param_name, const json & param_schema, bool is_required) {
                    auto rule_name = "tool-" + name + "-arg-" + param_name;

                    auto arg_open = "<arg_key>" + p.literal_tag(Tag::TOOL_ARG_NAME, param_name) + "</arg_key>\n<arg_value>";
                    auto arg_close = p.literal("</arg_value>\n");
                    auto arg_value = p.eps();

                    if (schema_info.resolves_to_string(param_schema)) {
                        arg_value = p.tag(Tag::TOOL_ARG_STRING_VALUE, arg_string);
                    } else {
                        arg_value = p.tag(Tag::TOOL_ARG_JSON_VALUE, p.schema(p.json(), rule_name + "-schema", param_schema));
                    }

                    auto arg_rule = p.rule(rule_name, p.atomic_tag(Tag::TOOL_ARG_OPEN, arg_open) + arg_value + p.atomic_tag(Tag::TOOL_ARG_CLOSE, arg_close));
                    args += p.repeat(arg_rule, /* min = */ 0, /* max = */ 1);
                });

                if (allow_additional) {
                    auto dynamic_key = p.literal("<arg_key>") + p.tag(Tag::TOOL_ARG_NAME, p.until("</arg_key>")) + p.literal("</arg_key>\n<arg_value>");
                    auto dynamic_close = p.literal("</arg_value>\n");
                    auto additional_value = p.choice();
                    if (additional_has_schema) {
                        if (schema_info.resolves_to_string(additional_schema)) {
                            additional_value |= p.tag(Tag::TOOL_ARG_STRING_VALUE, arg_string);
                        } else {
                            additional_value |= p.tag(Tag::TOOL_ARG_JSON_VALUE,
                                p.schema(p.json(), "glm-additional-" + name, additional_schema));
                        }
                    } else {
                        additional_value |= p.tag(Tag::TOOL_ARG_STRING_VALUE, arg_string);
                    }

                    auto additional_rule = p.rule("tool-" + name + "-arg-generic",
                        p.atomic_tag(Tag::TOOL_ARG_OPEN, dynamic_key)
                        + additional_value
                        + p.atomic_tag(Tag::TOOL_ARG_CLOSE, dynamic_close));
                    args += p.repeat(additional_rule, 0, -1);
                }

                tool_choice |= p.rule("tool-" + name, p.atomic_tag(Tag::TOOL_OPEN, tool_open) + args + p.atomic_tag(Tag::TOOL_CLOSE, tool_close));
            });

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_calls = p.trigger_rule("tool-call-root", p.repeat(tool_choice, /* min = */ min_calls, /* max = */ max_calls));

            // Content chunks are text until thinking or tool call markers
            auto content_chunk = p.optional(p.literal("\n")) + p.tag(Tag::CONTENT, p.until_one_of({"<think>", "\n<tool_call>", "<tool_call>"}));

            if (extract_reasoning) {
                auto mixed = p.zero_or_more(thinking_block | content_chunk);
                if (data.thinking_forced_open) {
                    return forced_thinking + mixed + tool_calls + mixed;
                }
                return mixed + tool_calls + mixed;
            }

            auto content_before = p.optional(p.literal("\n")) + p.tag(Tag::CONTENT, p.until_one_of({"\n<tool_call>", "<tool_call>"}));
            auto content_after = p.tag(Tag::CONTENT, p.rest());
            return content_before + tool_calls + content_after;
        }

        // Content only parser
        include_grammar = false;
        if (extract_reasoning) {
            // Mixed content with interleaved thinking
            auto content_chunk = p.optional(p.literal("\n")) + p.tag(Tag::CONTENT, p.until("<think>"));
            auto mixed = p.zero_or_more(thinking_block | content_chunk);
            if (data.thinking_forced_open) {
                return forced_thinking + mixed;
            }
            return mixed;
        }
        auto final_content = p.sequence();
        final_content += p.optional(p.literal("\n"));
        final_content += p.tag(Tag::CONTENT, p.rest());
        return final_content;
    });

    data.parser = parser.save();

    if (include_grammar) {
        data.grammar_lazy = has_tools && inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_AUTO;

        // Build grammar from PEG parser
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            foreach_function(inputs.tools, [&](const json & tool) {
                auto schema = tool.at("function").at("parameters");
                builder.resolve_refs(schema);
            });
            parser.build_grammar(builder, data.grammar_lazy);
        });

        if (data.grammar_lazy) {
            data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<tool_call>"});
        } else {
            data.grammar_triggers.clear();
        }
    }

    return data;
}
 254 changes: 254 additions & 0 deletions254  
common/chat-parsers/gpt-oss.cpp
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,254 @@
// GPT-OSS tool call format
// Uses channel-based messaging with special tokens:
// - <|channel|>analysis, <|channel|>commentary, <|channel|>final
// - <|message|>...content...<|end|>
// - <|start|>assistant
// Tool calls format:
// - In role: to=functions.name<|channel|>analysis|commentary<|message|>{...}
// - In channel: <|channel|>analysis|commentary to=functions.name<|message|>{...}

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_gpt_oss(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    // Copy reasoning to the "thinking" field as expected by the gpt-oss template
    auto adjusted_messages = json::array();
    for (const auto & msg : inputs.messages) {
        auto has_reasoning_content = msg.contains("reasoning_content") && msg.at("reasoning_content").is_string();
        auto has_tool_calls = msg.contains("tool_calls") && msg.at("tool_calls").is_array();

        if (has_reasoning_content && has_tool_calls) {
            auto adjusted_message = msg;
            adjusted_message["thinking"] = msg.at("reasoning_content");
            adjusted_messages.push_back(adjusted_message);
        } else {
            adjusted_messages.push_back(msg);
        }
    }

    auto prompt = apply(tmpl, inputs, /* messages_override= */ adjusted_messages);

    // Check if we need to replace the return token with end token during
    // inference and without generation prompt. For more details see:
    // https://github.com/ggml-org/llama.cpp/issues/15417
    if (inputs.is_inference && !inputs.add_generation_prompt) {
        static constexpr std::string_view return_token = "<|return|>";
        static constexpr std::string_view end_token    = "<|end|>";
        if (size_t pos = prompt.rfind(return_token); pos != std::string::npos) {
            prompt.replace(pos, return_token.length(), end_token);
        }
    }

    data.prompt = prompt;
    data.format = COMMON_CHAT_FORMAT_GPT_OSS;

    // These special tokens are required to parse properly, so we include them
    // even if parse_tool_calls is false.
    data.preserved_tokens = {
        "<|channel|>",
        "<|constrain|>",
        "<|message|>",
        "<|start|>",
        "<|end|>",
    };

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;

    // Build PEG parser for GPT-OSS format
    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;

        auto assistant_prefix = [&]() {
            return p.optional(p.token("<|start|>") + "assistant");
        };

        auto commentary_content = p.rule("gpt-oss-commentary",
            assistant_prefix()
            + p.token("<|channel|>") + "commentary"
            + p.token("<|message|>")
            + p.tag(Tag::CONTENT, p.until("<|end|>"))
            + p.token("<|end|>")
        );

        auto final_content = p.rule("gpt-oss-final",
            assistant_prefix()
            + p.token("<|channel|>") + "final"
            + p.optional(p.literal(" ") + p.token("<|constrain|>") + p.until("<|message|>"))
            + p.token("<|message|>")
            + p.tag(Tag::CONTENT, p.until("<|end|>"))
            + p.token("<|end|>")
        );

        auto reasoning_block = p.eps();
        if (extract_reasoning) {
            reasoning_block = p.optional(p.tag(Tag::REASONING,
                p.token("<|channel|>") + "analysis" + p.token("<|message|>") + p.until("<|end|>")) + p.token("<|end|>")
                + assistant_prefix()
            );
        }

        // Response format parser (with JSON schema constraint)
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            // Final channel with JSON content
            return reasoning_block << p.optional(p.token("<|channel|>") + "final") << p.optional(p.space())
                << p.optional(p.token("<|constrain|>") + p.until("<|message|>"))
                << p.token("<|message|>")
                << p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema));
        }

        // Tool call parser
        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            auto tool_choice = p.choice();

            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");

                // Tool call in channel: <|channel|>analysis|commentary to=functions.name<|message|>{...}
                tool_choice |= p.rule("tool-channel-" + name, p.tag(Tag::TOOL,
                    assistant_prefix()
                    + p.token_tag(Tag::TOOL_OPEN, "<|channel|>")
                    + (p.literal("analysis") | "commentary")
                    + " to=functions." + p.literal_tag(Tag::TOOL_NAME, name)
                    + p.optional(" " + p.token("<|constrain|>") + "json")
                    + p.token("<|message|>")
                    + p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "tool-" + name + "-params", parameters))
                ));

                // Tool call in role: to=functions.name<|channel|>analysis|commentary<|message|>{...}
                tool_choice |= p.rule("tool-role-" + name, p.tag(Tag::TOOL,
                    assistant_prefix()
                    + p.literal_tag(Tag::TOOL_OPEN, " to=functions.")
                    + p.literal_tag(Tag::TOOL_NAME, name)
                    + p.token("<|channel|>")
                    + (p.literal("analysis") | "commentary")
                    + p.optional(" " + p.token("<|constrain|>") + "json")
                    + p.token("<|message|>")
                    + p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "tool-" + name + "-params", parameters))
                ));
            });

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_calls = p.trigger_rule("tool-call", p.repeat(tool_choice, min_calls, max_calls));

            auto pre_tool_content = p.repeat(commentary_content, 0, -1);

            return reasoning_block << pre_tool_content << tool_calls;
        }

        // Content only parser with optional reasoning
        auto content_sequence = p.sequence();
        content_sequence += p.repeat(commentary_content, 0, -1);
        content_sequence += p.choice({final_content, commentary_content});

        return reasoning_block << content_sequence;
    });

    data.parser = parser.save();

    if (!inputs.json_schema.is_null()) {
        data.grammar_lazy = false;
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            auto schema = inputs.json_schema;
            builder.resolve_refs(schema);

            auto not_end = builder.add_rule("not-end",
                "[^<] | \"<\" [^|] | \"<|\" [^e] | \"<|e\" [^n] | \"<|en\" [^d] | \"<|end\" [^|] | \"<|end|\" [^>]");
            auto analysis = builder.add_rule("analysis",
                "\"<|channel|>analysis<|message|>\" ( " + not_end + " )* \"<|end|>\"");
            auto constraint = builder.add_rule("constraint", "\"<|constrain|>\"? [a-zA-Z0-9_-]+");
            auto final = builder.add_rule("final",
                "\"<|channel|>final\" ( \" \" " + constraint + " )? \"<|message|>\" " +
                builder.add_schema("response", schema)
            );

            builder.add_rule("root", "( " + analysis + " \"<|start|>assistant\" )? " + final);
        });
    }

    if (has_tools) {
        data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            // tool calls can appear in commentary or analysis channels
            auto channel = builder.add_rule("channel", "\"<|channel|>\" ( \"commentary\" | \"analysis\" )");

            std::vector<std::string> tool_rules_recipient_in_role;
            std::vector<std::string> tool_rules_recipient_in_channel;
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");
                builder.resolve_refs(parameters);

                tool_rules_recipient_in_role.push_back(
                    builder.add_rule(name + "-call",
                        "\"" + name + "\"" + channel + " \" <|constrain|>json\"? \"<|message|>\" " +
                        builder.add_schema(name + "-args", parameters)
                    )
                );

                tool_rules_recipient_in_channel.push_back(
                    builder.add_rule(name + "-call",
                        "\"" + name + "\"" + " \" <|constrain|>json\"? \"<|message|>\" " +
                        builder.add_schema(name + "-args", parameters)
                    )
                );
            });

            auto recipient_in_channel = builder.add_rule("recipient_in_channel",
                channel + " \" to=functions.\" ( " +
                string_join(tool_rules_recipient_in_channel, " | ") + " )"
            );

            if (data.grammar_lazy) {
                auto recipient_in_role = builder.add_rule("recipient_in_role",
                    "\"<|start|>assistant\"? \" to=functions.\" ( " +
                    string_join(tool_rules_recipient_in_role, " | ") + " )"
                );

                builder.add_rule("root", recipient_in_role + " | " + recipient_in_channel);
            } else {
                auto not_end = builder.add_rule("not-end",
                    "[^<] | \"<\" [^|] | \"<|\" [^e] | \"<|e\" [^n] | \"<|en\" [^d] | \"<|end\" [^|] | \"<|end|\" [^>]");
                auto analysis = builder.add_rule("analysis",
                    "\"<|channel|>analysis<|message|>\" ( " + not_end + " )* \"<|end|>\"");
                auto commentary = builder.add_rule("commentary",
                    "\"<|channel|>commentary<|message|>\" ( " + not_end + " )* \"<|end|>\"");

                auto recipient_in_role = builder.add_rule("recipient_in_role",
                    "\" to=functions.\" ( " + string_join(tool_rules_recipient_in_role, " | ") + " )"
                );

                builder.add_rule("root",
                    "( " + analysis + " \"<|start|>assistant\" )? " +
                    "( " + commentary + " \"<|start|>assistant\" )? " +
                    "( " + recipient_in_role + " | " + recipient_in_channel + " )"
                );
            }

            // Trigger on tool calls that appear in the commentary channel
            data.grammar_triggers.push_back({
                COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN,
                "<\\|channel\\|>(commentary|analysis) to"
            });

            // Trigger tool calls that appear in the role section, either at the
            // start or in the middle.
            data.grammar_triggers.push_back({
                COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL,
                "^ to"
            });

            data.grammar_triggers.push_back({
                COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN,
                "<\\|start\\|>assistant to"
            });
        });
    }

    return data;
}
 89 changes: 89 additions & 0 deletions89  
common/chat-parsers/granite.cpp
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,89 @@
// Granite tool call format
// Format: {"tool_calls": [{"name": "func", "arguments": {...}}], "content": "..."}
// With optional <think>...</think> and <response>...</response> tags

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_granite(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    // Pass thinking context for Granite template
    json additional_context = {
        {"thinking", inputs.enable_thinking},
    };

    data.prompt = apply(tmpl, inputs, /* messages_override= */ std::nullopt, /* tools_override= */ std::nullopt, additional_context);
    data.format = COMMON_CHAT_FORMAT_GRANITE;

    if (string_ends_with(data.prompt, "<think>\n") || string_ends_with(data.prompt, "<think>")) {
        if (!inputs.enable_thinking) {
            data.prompt += "</think>";
        } else {
            data.thinking_forced_open = true;
        }
    }

    data.preserved_tokens = {
        "<think>",
        "</think>",
        "<response>",
        "</response>",
        "<|end_of_text|>",
    };

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;
    auto include_grammar = true;

    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;

        auto consume_eot = [&]() {
            return p.optional(p.token("<|end_of_text|>")) + p.optional(p.space());
        };

        auto reasoning = p.eps();
        if (inputs.enable_thinking && extract_reasoning) {
            auto reasoning_content = p.tag(Tag::REASONING, p.until("</think>")) + ("</think>" | p.end());
            if (data.thinking_forced_open) {
                reasoning = reasoning_content;
            } else {
                reasoning = p.optional("<think>" + reasoning_content);
            }
        }

        // Response format parser
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            return reasoning << p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema));
        }

        // Tool call parser: Granite emits a JSON object with tool_calls + content fields
        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            auto payload = p.tag(Tag::TOOL_ARGS, p.json());
            return reasoning << p.optional(p.space()) << payload << consume_eot();
        }

        // Content-only parser: trim trailing <|end_of_text|> and optionally handle <response> blocks
        auto response_block = p.literal("<response>") + p.tag(Tag::CONTENT, p.until("</response>")) + (p.literal("</response>") | p.end());
        auto content_until_eot = p.tag(Tag::CONTENT, p.until("<|end_of_text|>")) << consume_eot();

        include_grammar = false;
        return reasoning << p.choice({response_block, content_until_eot, p.tag(Tag::CONTENT, p.rest())});
    });

    data.parser = parser.save();

    if (include_grammar) {
        data.grammar_lazy = has_tools && inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_AUTO;
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            parser.build_grammar(builder, data.grammar_lazy);
        });
        if (data.grammar_lazy) {
            data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, R"("tool_calls")"});
        } else {
            data.grammar_triggers.clear();
        }
    }

    return data;
}
 210 changes: 210 additions & 0 deletions210  
common/chat-parsers/hermes-2-pro.cpp
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,210 @@
// Hermes 2 Pro tool call format
// Formats:
// - <tool_call>{"name":"func","arguments":{}}</tool_call>
// - <function=name>{"key":"value"}</function>
// - <function name="name">{"key":"value"}</function>
// With optional <think>...</think> reasoning blocks

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_hermes_2_pro(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    json extra_context = json {
        {"enable_thinking", inputs.enable_thinking},
    };
    extra_context.update(inputs.extra_context);

    data.prompt = apply(tmpl, inputs, /* messages_override =*/ std::nullopt, /* tools_override= */ std::nullopt, extra_context);

    if (string_ends_with(data.prompt, "<think>\n")) {
        if (!extra_context["enable_thinking"]) {
            data.prompt += "</think>";
        } else {
            data.thinking_forced_open = true;
        }
    }

    bool has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;

    data.format = COMMON_CHAT_FORMAT_HERMES_2_PRO;
    data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;

    data.preserved_tokens = {
        "<think>",
        "</think>",
        "<tool_call>",
        "</tool_call>",
        "<function",
        "<tools>",
        "</tools>",
        "<response>",
        "</response>",
        "<function_call>",
        "</function_call>",
        "<json>",
        "</json>",
        "<JSON>",
        "</JSON>",
        "```",
        "```json",
        "```xml",
    };

    // Build PEG parser
    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;

        auto consume_message_end = [&]() {
            return p.optional(p.choice({p.literal("<|im_end|>"), p.literal("<|eot_id|>"), p.literal("<|eom_id|>")}))
                + p.optional(p.space());
        };

        // Optional thinking block
        auto reasoning = p.eps();
        if (extract_reasoning) {
            if (data.thinking_forced_open) {
                reasoning = p.tag(Tag::REASONING, p.until("</think>")) + "</think>";
            } else {
                reasoning = p.optional("<think>" + p.tag(Tag::REASONING, p.until("</think>")) + "</think>");
            }
        }

        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            auto tool_choice = p.choice();

            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");

                // <tool_call>{"name":"func","arguments":{}}</tool_call>
                tool_choice |= p.rule("tool-call-" + name, p.tag(Tag::TOOL,
                    p.token_tag(Tag::TOOL_OPEN, "<tool_call>")
                    + p.space()
                    + "{" + p.space()
                    + "\"name\"" + p.space() + ":" + p.space()
                    + "\"" + p.literal_tag(Tag::TOOL_NAME, name) + "\"" + p.space() + "," + p.space()
                    + "\"arguments\"" + p.space() + ":" + p.space()
                    + p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "tool-" + name + "-args", parameters))
                    + p.space() + "}"
                    + p.space()
                    + p.token_tag(Tag::TOOL_CLOSE, "</tool_call>")
                ) + p.space());

                // <function=name>{...}</function>
                tool_choice |= p.rule("func-eq-" + name, p.tag(Tag::TOOL,
                    p.atomic_tag(Tag::TOOL_OPEN, "<function=" + p.literal_tag(Tag::TOOL_NAME, name) + ">")
                    + p.space()
                    + p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "func-" + name + "-args", parameters))
                    + p.space()
                    + p.token_tag(Tag::TOOL_CLOSE, "</function>")
                ) + p.space());

                // <function name="name">{...}</function>
                tool_choice |= p.rule("func-name-" + name, p.tag(Tag::TOOL,
                    p.atomic_tag(Tag::TOOL_OPEN, "<function" + p.space() + "name=\"" + p.literal_tag(Tag::TOOL_NAME, name) + "\">")
                    + p.space()
                    + p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "funcn-" + name + "-args", parameters))
                    + p.space()
                    + p.token_tag(Tag::TOOL_CLOSE, "</function>")
                ) + p.space());
            });

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_calls = p.trigger_rule("tool-call", p.repeat(tool_choice, min_calls, max_calls));

            auto content_prefix = p.optional(p.tag(Tag::CONTENT, p.until_one_of({
                "<tool_call>",
                "<function",
            })));

            return reasoning << content_prefix << tool_calls << consume_message_end();
        }

        // Content only parser
        auto content_block = p.sequence({
            p.tag(Tag::CONTENT, p.until("<|im_end|>")),
            consume_message_end()
        });
        return reasoning << p.choice({content_block, p.tag(Tag::CONTENT, p.rest()), p.eps()});
    });

    data.parser = parser.save();

    if (has_tools) {
        // Build grammar manually for backward compatibility with streaming tests
        // (using regular string literals instead of token syntax)
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            std::vector<std::string> tool_rules;
            std::vector<std::string> tool_call_alts;
            std::vector<std::string> escaped_names;
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");
                builder.resolve_refs(parameters);
                tool_rules.push_back(builder.add_schema(name + "-call", {
                    {"type", "object"},
                    {"properties", json {
                        {"name", json {{"const", name}}},
                        {"arguments", parameters},
                    }},
                    {"required", json::array({"name", "arguments"})},
                }));
                tool_call_alts.push_back(builder.add_rule(
                    name + "-function-tag",
                    "\"<function\" ( \"=" + name + "\" | \" name=\\\"" + name + "\\\"\" ) \">\" space " +
                    builder.add_schema(name + "-args", parameters) + " "
                    "\"</function>\" space"));

                data.grammar_triggers.push_back({
                    COMMON_GRAMMAR_TRIGGER_TYPE_WORD,
                    "<function=" + name + ">",
                });
                escaped_names.push_back(regex_escape(name));
                data.grammar_triggers.push_back({
                    COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN,
                    "<function\\s+name\\s*=\\s*\"" + regex_escape(name) + "\"",
                });
            });
            auto any_tool_call = builder.add_rule("any_tool_call", "( " + string_join(tool_rules, " | ") + " ) space");
            std::vector<std::string> alt_tags {
                any_tool_call,
                "\"<tool_call>\" space "     + any_tool_call + " \"</tool_call>\"",
                // The rest is just to accommodate common "good bad" outputs.
                "\"<function_call>\" space " + any_tool_call + " \"</function_call>\"",
                "\"<response>\"  space "     + any_tool_call + " \"</response>\"",
                "\"<tools>\"     space "     + any_tool_call + " \"</tools>\"",
                "\"<json>\"      space "     + any_tool_call + " \"</json>\"",
                "\"<xml>\"      space "     + any_tool_call + " \"</xml>\"",
                "\"<JSON>\"      space "     + any_tool_call + " \"</JSON>\"",
            };
            auto wrappable_tool_call = builder.add_rule("wrappable_tool_call", "( " + string_join(alt_tags, " | ") + " ) space");
            tool_call_alts.push_back(wrappable_tool_call);
            tool_call_alts.push_back(
                "( \"```\\n\" | \"```json\\n\" | \"```xml\\n\" ) space " + wrappable_tool_call + " space \"```\" space ");
            auto tool_call = builder.add_rule("tool_call", string_join(tool_call_alts, " | "));
            builder.add_rule("root",
                std::string(data.thinking_forced_open ? "( \"</think>\" space )? " : "") +
                (inputs.parallel_tool_calls ? "(" + tool_call + ")+" : tool_call));
            // Trigger on some common known "good bad" outputs
            data.grammar_triggers.push_back({
                COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL,
                std::string(data.thinking_forced_open ? "[\\s\\S]*?(</think>\\s*)" : "(?:<think>[\\s\\S]*?</think>\\s*)?") + (
                    "\\s*("
                    "(?:<tool_call>"
                    "|<function"
                    "|(?:```(?:json|xml)?\n\\s*)?(?:<function_call>|<tools>|<xml><json>|<response>)?"
                    "\\s*\\{\\s*\"name\"\\s*:\\s*\"(?:" + string_join(escaped_names, "|") + ")\""
                    ")"
                    ")[\\s\\S]*"
                ),
            });
        });
    }

    return data;
}
 120 changes: 120 additions & 0 deletions120  
common/chat-parsers/kimi-k2.cpp
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,120 @@
// Kimi K2 tool call format
// Format: <|tool_calls_section_begin|><|tool_call_begin|>function_name<|tool_call_argument_begin|>{"key": value}<|tool_call_end|><|tool_calls_section_end|>
// With optional <think>...</think> reasoning blocks

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_kimi_k2(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_KIMI_K2;

    data.preserved_tokens = {
        "<think>",
        "</think>",
        "<|tool_calls_section_begin|>",
        "<|tool_call_begin|>",
        "<|tool_call_argument_begin|>",
        "<|tool_call_end|>",
        "<|tool_calls_section_end|>",
        "<|im_end|>",
        "<|im_system|>",
        "<|im_middle|>",
    };

    data.additional_stops.insert(data.additional_stops.end(), {
        "<|im_end|>",
        "<|im_middle|>"
    });

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;
    auto include_grammar = true;

    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;
        auto optional_newline = [&]() {
            return p.optional(p.literal("\n"));
        };

        auto reasoning = p.eps();
        if (inputs.enable_thinking && extract_reasoning) {
            auto reasoning_content = p.tag(Tag::REASONING, p.until("</think>")) + ("</think>" | p.end());
            reasoning = p.optional(optional_newline() + "<think>" + reasoning_content);
        }

        // Response format parser
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            return reasoning << p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema));
        }

        // Tool call parser
        // Format: <|tool_call_begin|>functions.{name}:{counter}<|tool_call_argument_begin|>{...}<|tool_call_end|>
        bool require_tools = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED;
        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            auto tool_choice = p.choice();

            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");

                // Match: functions.{name}:{id}
                // Use atomic_tag to ensure tool calls are only created when fully matched
                auto tool_open = p.token("<|tool_call_begin|>")
                    + "functions." + p.literal_tag(Tag::TOOL_NAME, name) + ":"
                    + p.tag(Tag::TOOL_ID, p.until("<|tool_call_argument_begin|>"))
                    + "<|tool_call_argument_begin|>";
                auto tool_close = p.token("<|tool_call_end|>");
                auto tool_args = p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "tool-" + name + "-args", parameters));

                tool_choice |= p.rule("tool-" + name,
                    p.atomic_tag(Tag::TOOL_OPEN, tool_open)
                    + tool_args
                    + p.atomic_tag(Tag::TOOL_CLOSE, tool_close));
            });

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_calls = p.trigger_rule("tool-call",
                "<|tool_calls_section_begin|>"
                + p.repeat(tool_choice, min_calls, max_calls)
                + "<|tool_calls_section_end|>"
            );

            auto content_before = optional_newline() + p.tag(Tag::CONTENT, p.until("<|tool_calls_section_begin|>"));
            auto content_after = optional_newline() + p.tag(Tag::CONTENT, p.rest());
            if (require_tools) {
                return reasoning << tool_calls;
            }
            return reasoning << content_before << tool_calls << content_after;
        }

        // Content only parser
        include_grammar = false;
        return reasoning << optional_newline() << p.tag(Tag::CONTENT, p.rest());
    });

    data.parser = parser.save();

    if (include_grammar) {
        data.grammar_lazy = has_tools && inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_AUTO;

        // Build grammar from PEG parser
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            foreach_function(inputs.tools, [&](const json & tool) {
                auto schema = tool.at("function").at("parameters");
                builder.resolve_refs(schema);
            });
            parser.build_grammar(builder, data.grammar_lazy);
        });
        if (data.grammar_lazy) {
            data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<|tool_calls_section_begin|>"});
        } else {
            data.grammar_triggers.clear();
        }
    }

    return data;
}
 120 changes: 120 additions & 0 deletions120  
common/chat-parsers/lfm2.cpp
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,120 @@
// LFM2 tool call format
// Format: <|tool_call_start|>[{"name": "...", "arguments": {...}}]<|tool_call_end|>

#include "chat-parsers-internal.h"

// Helper to find case-insensitive substring (same as in chat.cpp)
static size_t ifind_string(const std::string & str, const std::string & pattern) {
    auto it = std::search(
        str.begin(), str.end(),
        pattern.begin(), pattern.end(),
        [](char a, char b) { return std::tolower(a) == std::tolower(b); }
    );
    return it == str.end() ? std::string::npos : std::distance(str.begin(), it);
}

common_chat_params common_chat_params_init_lfm2(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;
    const auto is_json_schema_provided = !inputs.json_schema.is_null();
    const auto is_grammar_provided = !inputs.grammar.empty();
    const auto are_tools_provided = inputs.tools.is_array() && !inputs.tools.empty();

    // the logic requires potentially modifying the messages
    auto tweaked_messages = inputs.messages;

    auto replace_json_schema_marker = [](json & messages) -> bool {
        static std::string marker1 = "force json schema.\n";
        static std::string marker2 = "force json schema.";

        if (messages.empty() || messages.at(0).at("role") != "system") {
            return false;
        }

        std::string content = messages.at(0).at("content");

        for (const auto & marker : {marker1, marker2}) {
            const auto pos = ifind_string(content, marker);
            if (pos != std::string::npos) {
                content.replace(pos, marker.length(), "");
                // inject modified content back into the messages
                messages.at(0).at("content") = content;
                return true;
            }
        }

        return false;
    };

    // Lfm2 model does not natively work with json, but can generally understand the tools structure
    // For the llama server compatibility with json tools semantic,
    // the client can add "Follow json schema." line into the system message prompt to force the json output.
    if (are_tools_provided && (is_json_schema_provided || is_grammar_provided)) {
        // server/utils.hpp prohibits that branch for the custom grammar anyways
        throw std::runtime_error("Tools call must not use \"json_schema\" or \"grammar\", use non-tool invocation if you want to use custom grammar");
    } else if (are_tools_provided && replace_json_schema_marker(tweaked_messages)) {
        data.format = COMMON_CHAT_FORMAT_LFM2_WITH_JSON_TOOLS;
        data.preserved_tokens = {"<|tool_call_start|>", "<|tool_call_end|>"};

        // Build PEG parser
        auto parser = build_chat_peg_parser([&](auto & p) {
            using Tag = common_chat_peg_tag;

            // Tool call: <|tool_call_start|> + JSON array + <|tool_call_end|>
            auto tool_call = p.tag(Tag::TOOL,
                p.token_tag(Tag::TOOL_OPEN, "<|tool_call_start|>")
                + p.tag(Tag::TOOL_ARGS, p.json())
                + p.token_tag(Tag::TOOL_CLOSE, "<|tool_call_end|>")
            );

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_calls = p.trigger_rule("tool-call", p.repeat(tool_call, min_calls, max_calls));

            return p.tag(Tag::CONTENT, p.until("<|tool_call_start|>")) << tool_calls;
        });

        data.parser = parser.save();

        // Build grammar
        data.grammar_lazy = true;
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            auto schemas = json::array();
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                schemas.push_back({
                    {"type", "object"},
                    {"properties", {
                        {"name", {
                            {"type", "string"},
                            {"const", function.at("name")},
                        }},
                        {"arguments", function.at("parameters")},
                    }},
                    {"required", json::array({"name", "arguments", "id"})},
                });
            });
            auto schema = json{
                {"type", "array"},
                {"items", schemas.size() == 1 ? schemas[0] : json{{"anyOf", schemas}}},
                {"minItems", 1},
            };
            if (!inputs.parallel_tool_calls) {
                schema["maxItems"] = 1;
            }

            builder.add_rule("root", "\"<|tool_call_start|>\" " + builder.add_schema("tool_calls", schema) + " \"<|tool_call_end|>\"");
        });

        data.grammar_triggers = {{COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL, "\\s*<\\|tool_call_start\\|>\\s*\\["}};
    } else if (are_tools_provided && (!is_json_schema_provided && !is_grammar_provided)) {
        data.preserved_tokens = {"<|tool_call_start|>", "<|tool_call_end|>"};
    } else if (is_json_schema_provided) {
        data.grammar = json_schema_to_grammar(inputs.json_schema);
    } else if (is_grammar_provided) {
        data.grammar = inputs.grammar;
    }

    data.prompt = apply(tmpl, inputs, /* messages_override= */ tweaked_messages);

    return data;
}
 157 changes: 157 additions & 0 deletions157  
common/chat-parsers/llama-3-x.cpp
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,157 @@
// Llama 3.x tool call format
// Format: {"type":"function","name":"func","parameters":{...}}
// Also supports builtin tools: <|python_tag|>python.call(code="...")

#include "chat-parsers-internal.h"

static void expect_tool_parameters(const std::string & name, const json & parameters, const std::vector<std::string> & expected_properties) {
    if (!parameters.contains("properties") || !parameters.at("properties").is_object()) {
        throw std::runtime_error("Tool " + name + " is missing properties");
    }
    const auto & props = parameters.at("properties");
    for (const auto & prop_name : expected_properties) {
        if (!props.contains(prop_name)) {
            throw std::runtime_error("Tool " + name + " is missing property: " + prop_name);
        }
    }
}

common_chat_params common_chat_params_init_llama_3_x(const common_chat_template & tmpl, const struct templates_params & inputs, bool allow_python_tag_builtin_tools) {
    auto builtin_tools = json::array();
    common_chat_params data;

    bool has_tools = inputs.tools.is_array() && !inputs.tools.empty();

    if (has_tools) {
        data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;
        data.format = COMMON_CHAT_FORMAT_LLAMA_3_X;

        data.preserved_tokens = {};
        if (allow_python_tag_builtin_tools) {
            data.preserved_tokens.push_back("<|python_tag|>");
        }

        // Build PEG parser
        auto parser = build_chat_peg_parser([&](auto & p) {
            using Tag = common_chat_peg_tag;

            const auto consume_message_end = [&]() {
                auto seq = p.sequence();
                seq += p.optional(p.choice({
                    p.literal("<|eot_id|>"),
                    p.literal("<|eom_id|>"),
                    p.literal("<|end|>")
                }));
                seq += p.optional(p.space());
                return seq;
            };

            // Build tool call alternatives
            auto tool_choice = p.choice();

            // Check for builtin tools
            std::vector<std::string> builtin_tool_names;

            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");

                // Check if this is a builtin tool
                if (allow_python_tag_builtin_tools) {
                    if (name == "wolfram_alpha" || name == "web_search" || name == "brave_search" ||
                        name == "python" || name == "code_interpreter") {
                        builtin_tool_names.push_back(name);
                        builtin_tools.push_back(name);

                        // Builtin tool format: <|python_tag|>name.call(key="value")
                        common_peg_parser args = p.eps();
                        if (parameters.contains("properties")) {
                            bool first = true;
                            for (auto it = parameters.at("properties").begin(); it != parameters.at("properties").end(); ++it) {
                                if (!first) {
                                    args = args + ", ";
                                }
                                // Use constructed mapper tags: TOOL_ARG_NAME and TOOL_ARG_JSON_VALUE
                                args = args + p.literal_tag(Tag::TOOL_ARG_NAME, it.key()) + "=" + p.tag(Tag::TOOL_ARG_JSON_VALUE, p.json_string());
                                first = false;
                            }
                        }

                        tool_choice |= p.rule("builtin-" + name, p.tag(Tag::TOOL,
                            p.atomic_tag(Tag::TOOL_OPEN, p.token("<|python_tag|>") + p.literal_tag(Tag::TOOL_NAME, name) + ".call(")
                            + args
                            + p.literal_tag(Tag::TOOL_CLOSE, ")")
                        ));
                    }
                }

                // Standard JSON format: {"type":"function","name":"name","parameters":{...}}
                tool_choice |= p.rule("tool-" + name, p.tag(Tag::TOOL,
                    p.literal_tag(Tag::TOOL_OPEN, "{")
                    + p.optional("\"type\"" + p.space() + ":" + p.space() + "\"function\"" + p.space() + "," + p.space())
                    + "\"name\"" + p.space() + ":" + p.space()
                    + "\"" + p.literal_tag(Tag::TOOL_NAME, name) + "\"" + p.space() + "," + p.space()
                    + "\"parameters\"" + p.space() + ":" + p.space()
                    + p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "tool-" + name + "-params", parameters))
                    + p.atomic_tag(Tag::TOOL_CLOSE, p.space() + "}")
                ));
            });

            if (inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
                auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
                auto max_calls = inputs.parallel_tool_calls ? -1 : 1;

                // Content until we see start of JSON object or python_tag
                std::vector<std::string> delimiters = {"{"};
                if (!builtin_tool_names.empty()) {
                    delimiters.push_back("<|python_tag|>");
                }
                auto content = p.tag(Tag::CONTENT, p.until_one_of(delimiters)) << consume_message_end();
                auto tool_calls = p.trigger_rule("tool-call", p.repeat(tool_choice, min_calls, max_calls));

                return content << tool_calls;
            }

            // Content only parser
            auto content_only = p.sequence({
                p.tag(Tag::CONTENT, p.until_one_of({"<|eot_id|>", "<|eom_id|>", "<|end|>"})),
                consume_message_end()
            });
            return p.choice({content_only, p.tag(Tag::CONTENT, p.rest())});
        });

        data.parser = parser.save();

        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                auto schema = function.at("parameters");
                builder.resolve_refs(schema);
            });
            parser.build_grammar(builder, data.grammar_lazy);
        });

        // Grammar triggers
        data.grammar_triggers.push_back({
            COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL,
            "(\\{\\s*(?:\"type\"\\s*:\\s*\"function\"\\s*,\\s*)?\"name\"\\s*:\\s*\")[\\s\\S]*",
        });
        if (!builtin_tools.empty()) {
            data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<|python_tag|>"});
            data.format = COMMON_CHAT_FORMAT_LLAMA_3_X_WITH_BUILTIN_TOOLS;
        }

        data.additional_stops.push_back("<|eom_id|>");
    } else {
        data.format = COMMON_CHAT_FORMAT_CONTENT_ONLY;
    }

    data.prompt = apply(tmpl, inputs, /* messages_override =*/ std::nullopt, /* tools_override= */ std::nullopt, json {
        {"date_string", format_time(inputs.now, "%d %b %Y")},
        {"tools_in_user_message", false},
        {"builtin_tools", builtin_tools.empty() ? json() : builtin_tools},
    });

    return data;
}
 104 changes: 104 additions & 0 deletions104  
common/chat-parsers/magistral.cpp
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,104 @@
// Magistral tool call format
// Format: [THINK]...[/THINK][TOOL_CALLS][{"name":"func","arguments":{},"id":"abc123def"}]

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_magistral(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_MAGISTRAL;

    data.preserved_tokens = {
        "[THINK]",
        "[/THINK]",
    };

    bool has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;

    // Build the PEG parser
    bool require_tools = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED;
    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;

        // Optional reasoning block
        auto reasoning = extract_reasoning
            ? p.optional("[THINK]" + p.tag(Tag::REASONING, p.until("[/THINK]")) + "[/THINK]")
            : p.eps();

        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            // Tool call parser: content followed by [TOOL_CALLS] and JSON array
            auto tool_call = p.tag(Tag::TOOL,
                p.token_tag(Tag::TOOL_OPEN, "[TOOL_CALLS]")
                + p.tag(Tag::TOOL_ARGS, p.json())
            );

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_calls = p.trigger_rule("tool-call", p.repeat(tool_call, min_calls, max_calls));

            if (require_tools) {
                return reasoning << tool_calls;
            }
            return reasoning << p.tag(Tag::CONTENT, p.until("[TOOL_CALLS]")) << tool_calls;
        }

        // Content only parser
        return reasoning << p.tag(Tag::CONTENT, p.rest());
    });

    data.parser = parser.save();

    if (has_tools) {
        data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            auto schemas = json::array();
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                schemas.push_back({
                    {"type", "object"},
                    {"properties", {
                        {"name", {
                            {"type", "string"},
                            {"const", function.at("name")},
                        }},
                        {"arguments", function.at("parameters")},
                        {"id", {
                            {"type", "string"},
                            {"pattern", "^[a-zA-Z0-9]{9}$"},
                        }},
                    }},
                    {"required", json::array({"name", "arguments", "id"})},
                });
            });
            auto schema = json {
                {"type", "array"},
                {"items", schemas.size() == 1 ? schemas[0] : json {{"anyOf", schemas}}},
                {"minItems", 1},
            };
            if (!inputs.parallel_tool_calls) {
                schema["maxItems"] = 1;
            }
            builder.add_rule("root", "\"[TOOL_CALLS]\" " + builder.add_schema("tool_calls", schema));
        });
        if (data.grammar_lazy) {
            data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "[TOOL_CALLS]"});
        } else {
            data.grammar_triggers.clear();
        }
        data.preserved_tokens.push_back("[TOOL_CALLS]");
    } else {
        data.grammar_lazy = false;
        if (!inputs.json_schema.is_null()) {
            if (!inputs.grammar.empty()) {
                throw std::runtime_error("Either \"json_schema\" or \"grammar\" can be specified, but not both");
            }
            data.grammar = json_schema_to_grammar(inputs.json_schema);
        } else {
            data.grammar = inputs.grammar;
        }
    }

    return data;
}
 227 changes: 227 additions & 0 deletions227  
common/chat-parsers/minimax-m2.cpp
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,227 @@
// MiniMax-M2 tool call format
// Format: <minimax:tool_call><invoke name="function"><parameter name="key">value</parameter></invoke></minimax:tool_call>
// With optional <think>...</think> reasoning blocks

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_minimax_m2(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_MINIMAX_M2;

    // Handle thinking tags based on prompt ending
    if (string_ends_with(data.prompt, "<think>\n")) {
        if (!inputs.enable_thinking) {
            data.prompt += "</think>\n\n";
        } else {
            data.thinking_forced_open = true;
        }
    }

    data.preserved_tokens = {
        "<think>",
        "</think>",
        "<minimax:tool_call>",
        "</minimax:tool_call>",
        "<invoke name=",
        "</invoke>",
        "<parameter name=",
        "</parameter>",
    };

    data.additional_stops.push_back("[e~[");

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;
    auto include_grammar = true;

    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;
        auto consume_footer = [&]() {
            return p.optional(p.literal("[e~[")) + p.optional(p.space());
        };
        auto reasoning = p.eps();
        if (inputs.enable_thinking && extract_reasoning) {
            auto reasoning_content = p.tag(Tag::REASONING, p.until("</think>")) + ("</think>" | p.end());
            if (data.thinking_forced_open) {
                reasoning = reasoning_content;
            } else {
                auto reasoning_block = p.choice({
                    p.literal("<think>") + reasoning_content,
                    reasoning_content,
                });
                reasoning = p.optional(reasoning_block);
            }
        }

        // Response format parser
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            return reasoning << p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema));
        }

        // Tool call parser
        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            auto invoke_choice = p.choice();
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");

                auto schema_info = common_schema_info();
                schema_info.resolve_refs(parameters);

                // Format: <invoke name="function_name"><parameter name="key">value</parameter></invoke>
                auto tool_open = "<invoke name=\"" + p.literal_tag(Tag::TOOL_NAME, name) + "\">" + p.space();
                auto tool_close = p.space() + p.literal("</invoke>") + p.space();

                auto arg_string = p.rule("xml-arg-string", p.until_one_of({
                    "</parameter>",
                    "<parameter name=",
                    "</invoke>"
                }));

                auto parameter_choice = p.choice();
                bool has_parameter_rules = false;

                auto arg_close = p.literal("</parameter>") + p.space();

                foreach_parameter(function, [&](const auto & param_name, const json & param_schema, bool /* is_required */) {
                    auto rule_name = "tool-" + name + "-arg-" + param_name;

                    auto arg_open = "<parameter name=\"" + p.literal_tag(Tag::TOOL_ARG_NAME, param_name) + "\">";
                    auto arg_value = p.eps();

                    if (schema_info.resolves_to_string(param_schema)) {
                        arg_value = p.tag(Tag::TOOL_ARG_STRING_VALUE, arg_string);
                    } else {
                        arg_value = p.tag(Tag::TOOL_ARG_JSON_VALUE,
                            p.schema(p.json(), rule_name + "-schema", param_schema));
                    }

                    auto arg_rule = p.rule(rule_name,
                        p.atomic_tag(Tag::TOOL_ARG_OPEN, arg_open)
                        + arg_value
                        + p.atomic_tag(Tag::TOOL_ARG_CLOSE, arg_close));
                    parameter_choice |= arg_rule;
                    has_parameter_rules = true;
                });

                bool allow_additional = false;
                bool additional_has_schema = false;
                json additional_schema;
                if (parameters.contains("additionalProperties")) {
                    const auto & additional = parameters.at("additionalProperties");
                    if (additional.is_boolean()) {
                        allow_additional = additional.get<bool>();
                    } else if (additional.is_object()) {
                        allow_additional = true;
                        additional_has_schema = true;
                        additional_schema = additional;
                    }
                }

                if (allow_additional || !has_parameter_rules) {
                    auto dynamic_key = "<parameter name=\"" + p.tag(Tag::TOOL_ARG_NAME, p.until("\"")) + "\">";
                    auto additional_value = p.choice();
                    if (additional_has_schema) {
                        if (schema_info.resolves_to_string(additional_schema)) {
                            additional_value |= p.tag(Tag::TOOL_ARG_STRING_VALUE, arg_string);
                        } else {
                            additional_value |= p.tag(Tag::TOOL_ARG_JSON_VALUE,
                                p.schema(p.json(), "tool-" + name + "-arg-generic", additional_schema));
                        }
                    } else {
                        additional_value |= p.tag(Tag::TOOL_ARG_STRING_VALUE, arg_string);
                    }

                    auto additional_rule = p.rule("tool-" + name + "-arg-generic",
                        p.atomic_tag(Tag::TOOL_ARG_OPEN, dynamic_key)
                        + additional_value
                        + p.atomic_tag(Tag::TOOL_ARG_CLOSE, arg_close));
                    parameter_choice |= additional_rule;
                    has_parameter_rules = true;
                }

                common_peg_parser args = has_parameter_rules ? p.repeat(parameter_choice, 0, -1) : p.eps();

                invoke_choice |= p.rule("tool-" + name, p.tag(Tag::TOOL,
                    p.atomic_tag(Tag::TOOL_OPEN, tool_open)
                    + args
                    + p.atomic_tag(Tag::TOOL_CLOSE, tool_close)));
            });

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_block = p.rule("tool-call-block",
                p.literal("<minimax:tool_call>")
                + p.space()
                + p.repeat(invoke_choice, /* min = */ 1, /* max = */ -1)
                + p.literal("</minimax:tool_call>")
                + p.space());
            auto tool_calls = p.trigger_rule("tool-call-root", p.repeat(tool_block, /* min = */ min_calls, /* max = */ max_calls));

            auto stop_before = std::vector<std::string> {
                "\n<minimax:tool_call>", "<minimax:tool_call>",
                "\n<TOOLCALL>", "<TOOLCALL>",
                "\n<SPECIAL_12>", "<SPECIAL_12>",
                "\n<SPECIAL_11>Assistant", "<SPECIAL_11>Assistant",
                "\n<SPECIAL_11>User", "<SPECIAL_11>User",
                "\n<SPECIAL_10>System", "<SPECIAL_10>System",
            };
            auto stop_after = std::vector<std::string> {
                "\n<SPECIAL_12>", "<SPECIAL_12>",
                "\n<TOOLCALL>", "<TOOLCALL>",
                "\n<SPECIAL_11>Assistant", "<SPECIAL_11>Assistant",
                "\n<SPECIAL_11>User", "<SPECIAL_11>User",
                "\n<SPECIAL_10>System", "<SPECIAL_10>System",
                "\n<minimax:tool_call>", "<minimax:tool_call>",
            };
            auto content_before = p.optional(p.tag(Tag::CONTENT, p.until_one_of(stop_before)));
            auto content_after = p.optional(p.choice({
                p.sequence({p.tag(Tag::CONTENT, p.until_one_of(stop_after)), consume_footer()}),
                p.tag(Tag::CONTENT, p.rest())
            }));
            return reasoning << content_before << tool_calls << content_after;
        }

        // Content only parser
        include_grammar = false;
        auto stop_only = std::vector<std::string> {
            "\n<SPECIAL_12>", "<SPECIAL_12>",
            "\n<minimax:tool_call>", "<minimax:tool_call>",
            "\n<TOOLCALL>", "<TOOLCALL>",
            "\n<SPECIAL_11>Assistant", "<SPECIAL_11>Assistant",
            "\n<SPECIAL_11>User", "<SPECIAL_11>User",
            "\n<SPECIAL_10>System", "<SPECIAL_10>System",
        };
        auto content_tail = p.choice({
            p.sequence({p.tag(Tag::CONTENT, p.until_one_of(stop_only)), consume_footer()}),
            p.tag(Tag::CONTENT, p.rest())
        });
        return reasoning << content_tail;
    });

    data.parser = parser.save();

    if (include_grammar) {
        data.grammar_lazy = has_tools && inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_AUTO;

        // Build grammar from PEG parser
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            foreach_function(inputs.tools, [&](const json & tool) {
                auto schema = tool.at("function").at("parameters");
                builder.resolve_refs(schema);
            });
            parser.build_grammar(builder, data.grammar_lazy);
        });

        if (data.grammar_lazy) {
            data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<minimax:tool_call>"});
        } else {
            data.grammar_triggers.clear();
        }
    }

    return data;
}
 130 changes: 130 additions & 0 deletions130  
common/chat-parsers/ministral-3.cpp
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,130 @@
// Ministral/Mistral Large 3 tool call format
// Format: [TOOL_CALLS]name[ARGS]{"param": value}
// With optional [THINK]...[/THINK] reasoning blocks

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_ministral_3(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    // Build up messages to follow the format: https://huggingface.co/mistralai/Ministral-3-14B-Reasoning-2512/blob/main/chat_template.jinja
    auto adjusted_messages = json::array();
    for (const auto & msg : inputs.messages) {
        auto role = msg.value("role", "");
        if (role != "system" && role != "assistant") {
            // Only adjust system and assistant messages. Interestingly, the system message may contain thinking.
            adjusted_messages.push_back(msg);
            continue;
        }

        auto content = json::array();

        // If message contains `reasoning_content`, add it as a block of type `thinking`
        if (msg.contains("reasoning_content") && msg.at("reasoning_content").is_string()) {
            content.push_back({
                {"type", "thinking"},
                {"thinking", msg.at("reasoning_content").get<std::string>()},
            });
        }

        // If message contains `content`, add it as a block of type `text`
        if (msg.contains("content")) {
            if (msg.at("content").is_string()) {
                content.push_back({
                    {"type", "text"},
                    {"text", msg.at("content").get<std::string>()},
                });
            } else if (msg.at("content").is_array()) {
                auto blocks = msg.at("content");
                content.insert(content.end(), blocks.begin(), blocks.end());
            }
        }

        auto adjusted = msg;
        adjusted["content"] = content;
        adjusted.erase("reasoning_content");
        adjusted_messages.push_back(adjusted);
    }

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;
    auto include_grammar = true;

    data.prompt = apply(tmpl, inputs, /* messages_override = */ adjusted_messages);
    data.format = COMMON_CHAT_FORMAT_MINISTRAL_3;
    data.preserved_tokens = {
        "[THINK]",
        "[/THINK]",
        "[TOOL_CALLS]",
        "[ARGS]",
    };

    bool require_tools = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED;
    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;
        auto reasoning = extract_reasoning ? p.optional("[THINK]" + p.tag(Tag::REASONING, p.until("[/THINK]")) + "[/THINK]") : p.eps();

        // Response format parser
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            // Ministral wants to emit json surrounded by code fences
            return reasoning << "```json" << p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema)) << "```";
        }

        // Tool call parser
        // Format: [TOOL_CALLS]func1[ARGS]{...}[TOOL_CALLS]func2[ARGS]{...}
        // Note: [TOOL_CALLS] prefix appears before EACH tool call
        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            auto tool_choice = p.choice();
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                const auto & schema = function.at("parameters");

                // Each tool call starts with [TOOL_CALLS] prefix
                tool_choice |= p.rule("tool-" + name, p.tag(Tag::TOOL,
                    p.token("[TOOL_CALLS]")
                    + p.atomic_tag(Tag::TOOL_OPEN, p.literal_tag(Tag::TOOL_NAME, name) + p.token("[ARGS]"))
                    + p.tag(Tag::TOOL_ARGS, p.schema(p.json(), "tool-" + name + "-schema", schema))
                ));
            });

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_calls = p.trigger_rule("tool-call", p.repeat(tool_choice, min_calls, max_calls));

            if (require_tools) {
                return reasoning << tool_calls;
            }
            return reasoning << p.tag(Tag::CONTENT, p.until("[TOOL_CALLS]")) << tool_calls;
        }

        // Content only parser
        include_grammar = false;
        return reasoning << p.tag(Tag::CONTENT, p.rest());
    });

    data.parser = parser.save();

    if (include_grammar) {
        data.grammar_lazy = has_tools && inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_AUTO;

        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                auto schema = function.at("parameters");
                builder.resolve_refs(schema);
            });
            parser.build_grammar(builder, data.grammar_lazy);
        });

        if (data.grammar_lazy) {
            data.grammar_triggers = {
                {COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "[TOOL_CALLS]"}
            };
        } else {
            data.grammar_triggers.clear();
        }
    }

    return data;
}
 80 changes: 80 additions & 0 deletions80  
common/chat-parsers/mistral-nemo.cpp
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,80 @@
// Mistral Nemo tool call format
// Format: [TOOL_CALLS][{"name":"func","arguments":{},"id":"abc123def"}]

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_mistral_nemo(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;
    data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;

    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_MISTRAL_NEMO;

    data.preserved_tokens = {
        "[TOOL_CALLS]",
    };

    bool has_tools = inputs.tools.is_array() && !inputs.tools.empty();

    // Build the PEG parser
    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;

        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            // Tool call parser: content followed by [TOOL_CALLS] and JSON array
            auto tool_call = p.tag(Tag::TOOL,
                p.token_tag(Tag::TOOL_OPEN, "[TOOL_CALLS]")
                + p.tag(Tag::TOOL_ARGS, p.json())
            );

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_calls = p.trigger_rule("tool-call", p.repeat(tool_call, min_calls, max_calls));

            return p.tag(Tag::CONTENT, p.until("[TOOL_CALLS]")) << tool_calls;
        }

        // Content only parser
        return p.tag(Tag::CONTENT, p.rest());
    });

    data.parser = parser.save();

    if (has_tools) {
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            auto schemas = json::array();
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                schemas.push_back({
                    {"type", "object"},
                    {"properties", {
                        {"name", {
                            {"type", "string"},
                            {"const", function.at("name")},
                        }},
                        {"arguments", function.at("parameters")},
                        {"id", {
                            {"type", "string"},
                            // Nemo's template expects a 9-character alphanumeric ID.
                            {"pattern", "^[a-zA-Z0-9]{9}$"},
                        }},
                    }},
                    {"required", json::array({"name", "arguments", "id"})},
                });
            });
            auto schema = json {
                {"type", "array"},
                {"items", schemas.size() == 1 ? schemas[0] : json {{"anyOf", schemas}}},
                {"minItems", 1},
            };
            if (!inputs.parallel_tool_calls) {
                schema["maxItems"] = 1;
            }
            builder.add_rule("root", "\"[TOOL_CALLS]\" " + builder.add_schema("tool_calls", schema));
        });

        data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "[TOOL_CALLS]"});
    }

    return data;
}
 157 changes: 157 additions & 0 deletions157  
common/chat-parsers/nemotron-v2.cpp
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,157 @@
// Nemotron v2 tool call format
// Format: <TOOLCALL>[{"name": "...", "arguments": {...}}]</TOOLCALL>
// With optional <think>...</think> reasoning blocks

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_nemotron_v2(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_NEMOTRON_V2;

    // Handle thinking tags appropriately based on inputs.enable_thinking
    if (string_ends_with(data.prompt, "<think>\n")) {
        if (!inputs.enable_thinking) {
            data.prompt += "</think>";
        } else {
            data.thinking_forced_open = true;
        }
    }

    data.preserved_tokens = {
        "<think>",
        "</think>",
        "<TOOLCALL>",
        "</TOOLCALL>",
        "<SPECIAL_12>",
        "<SPECIAL_11>Assistant",
        "<SPECIAL_11>User",
        "<SPECIAL_10>System",
    };


    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;
    auto include_grammar = true;

    bool require_tools = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED;
    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;
        auto skip_special_markers = [&]() {
            auto marker = p.rule("nemotron-special-marker",
                p.optional(p.literal("\n"))
                + p.choice({
                    p.literal("<SPECIAL_12>"),
                    p.literal("<SPECIAL_11>Assistant"),
                    p.literal("<SPECIAL_11>User"),
                    p.literal("<SPECIAL_10>System")
                })
                + p.optional(p.literal("\n"))
            );
            return p.repeat(marker, 0, -1);
        };

        auto reasoning = p.eps();
        if (inputs.enable_thinking && extract_reasoning) {
            auto reasoning_content = p.tag(Tag::REASONING, p.until("</think>")) + ("</think>" | p.end());
            if (data.thinking_forced_open) {
                reasoning = reasoning_content;
            }
        }

        // Response format parser
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            return reasoning << p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema));
        }

        // Tool call parser - JSON array format
        // Format: <TOOLCALL>[{"name": "...", "arguments": {...}}]</TOOLCALL>
        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            // Tool call: <TOOLCALL> + JSON array + </TOOLCALL>
            auto tool_call = p.tag(Tag::TOOL,
                p.token_tag(Tag::TOOL_OPEN, "<TOOLCALL>")
                + p.tag(Tag::TOOL_ARGS, p.json())
                + p.token_tag(Tag::TOOL_CLOSE, "</TOOLCALL>")
            );

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_calls = p.trigger_rule("tool-call", p.repeat(tool_call, min_calls, max_calls));

            auto specials = skip_special_markers();
            if (require_tools) {
                return reasoning << specials << tool_calls << specials;
            }
            auto stop_before = std::vector<std::string> {
                "\n<TOOLCALL>", "<TOOLCALL>",
                "\n<SPECIAL_12>", "<SPECIAL_12>",
                "\n<SPECIAL_11>Assistant", "<SPECIAL_11>Assistant",
                "\n<SPECIAL_11>User", "<SPECIAL_11>User",
                "\n<SPECIAL_10>System", "<SPECIAL_10>System",
            };
            auto stop_after = std::vector<std::string> {
                "\n<SPECIAL_12>", "<SPECIAL_12>",
                "\n<SPECIAL_11>Assistant", "<SPECIAL_11>Assistant",
                "\n<SPECIAL_11>User", "<SPECIAL_11>User",
                "\n<SPECIAL_10>System", "<SPECIAL_10>System",
            };
            auto content_before = p.optional(p.tag(Tag::CONTENT, p.until_one_of(stop_before)));
            auto content_after = (p.optional(p.tag(Tag::CONTENT, p.until_one_of(stop_after))) << specials);
            return reasoning << specials << content_before << specials << tool_calls << specials << content_after;
        }

        // Content only parser
        include_grammar = false;
        auto stop_only = std::vector<std::string> {
            "\n<SPECIAL_12>", "<SPECIAL_12>",
            "\n<SPECIAL_11>Assistant", "<SPECIAL_11>Assistant",
            "\n<SPECIAL_11>User", "<SPECIAL_11>User",
            "\n<SPECIAL_10>System", "<SPECIAL_10>System",
        };
        return reasoning << skip_special_markers() << p.tag(Tag::CONTENT, p.until_one_of(stop_only)) << skip_special_markers();
    });

    data.parser = parser.save();

    if (include_grammar) {
        data.grammar_lazy = has_tools && inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_AUTO;

        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            auto schemas = json::array();
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                schemas.push_back({
                    {"type", "object"},
                    {"properties", {
                        {"name", {
                            {"type", "string"},
                            {"const", function.at("name")},
                        }},
                        {"arguments", function.at("parameters")},
                    }},
                    {"required", json::array({"name", "arguments"})},
                });
            });
            auto schema = json{
                {"type", "array"},
                {"items", schemas.size() == 1 ? schemas[0] : json{{"anyOf", schemas}}},
                {"minItems", 1},
            };
            if (!inputs.parallel_tool_calls) {
                schema["maxItems"] = 1;
            }
            builder.add_rule("root", "\"<TOOLCALL>\" " + builder.add_schema("tool_calls", schema) + " \"</TOOLCALL>\"");
        });

        if (data.grammar_lazy) {
            data.grammar_triggers = {
                {COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<TOOLCALL>"}
            };
        } else {
            data.grammar_triggers.clear();
        }
    }

    return data;
}
 147 changes: 147 additions & 0 deletions147  
common/chat-parsers/nemotron-v3.cpp
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,147 @@
// Nemotron 3 Nano 30B A3B tool call format
// Format: <tool_call><function=name><parameter=key>value</parameter></function></tool_call>
// With optional <think>...</think> reasoning blocks

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_nemotron_v3(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_NEMOTRON_V3;

    // Handle thinking tags appropriately based on inputs.enable_thinking
    if (string_ends_with(data.prompt, "<think>\n")) {
        if (!inputs.enable_thinking) {
            data.prompt += "</think>";
        } else {
            data.thinking_forced_open = true;
        }
    }

    data.preserved_tokens = {
        "<think>",
        "</think>",
        "<tool_call>",
        "</tool_call>",
        "<toolcall>",
        "</toolcall>",
        "<SPECIAL_11>Assistant",
        "<SPECIAL_11>User",
        "<SPECIAL_12>",
    };

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;
    auto include_grammar = true;

    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;
        auto newline = p.choice({p.literal("\r\n"), p.literal("\n")});
        auto whitespace = p.repeat(p.choice({newline, p.literal(" "), p.literal("\t")}), 0, -1);
        auto skip_blank_lines = whitespace;
        auto assistant_header = p.literal("<|im_start|>assistant") + p.choice({p.literal("\r\n"), p.literal("\n")});
        auto assistant_prefix = whitespace + p.optional(assistant_header);
        auto assistant_suffix = whitespace + p.optional(p.literal("<|im_end|>")) + whitespace;
        auto after_reasoning_gap = whitespace;
        auto think_open = p.literal("<think>") + p.optional(newline);
        auto think_close = p.literal("</think>");
        auto reasoning = p.eps();
        if (inputs.enable_thinking && extract_reasoning) {
            auto reasoning_content = p.tag(Tag::REASONING, p.until("</think>")) + think_close;
            if (data.thinking_forced_open) {
                reasoning = reasoning_content;
            } else {
                reasoning = p.optional(think_open + reasoning_content);
            }
        } else {
            reasoning = p.optional(think_open + p.until("</think>") + think_close);
        }

        // Response format parser
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            return assistant_prefix + reasoning + p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema)) + assistant_suffix;
        }

        // Tool call parser
        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            auto tool_choice = p.choice();
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");

                auto tool_open = "<function=" + p.literal_tag(Tag::TOOL_NAME, name) + ">\n";
                auto tool_close = p.literal("</function>\n");
                auto arg_body = p.rule("nemotron-v3-arg-body", p.until_one_of({
                    "\n</parameter>",
                    "\n<parameter=",
                    "\n</function>"
                }));
                auto generic_arg = p.rule("tool-" + name + "-arg-generic",
                    p.atomic_tag(Tag::TOOL_ARG_OPEN,
                        p.literal("<parameter=")
                        + p.tag(Tag::TOOL_ARG_NAME, p.until(">"))
                        + p.literal(">\n"))
                    + p.tag(Tag::TOOL_ARG_STRING_VALUE, arg_body)
                    + p.optional(newline)
                    + p.optional(p.atomic_tag(Tag::TOOL_ARG_CLOSE, p.literal("</parameter>\n"))));
                auto args = p.repeat(generic_arg, 0, -1);

                tool_choice |= p.rule("tool-" + name, p.atomic_tag(Tag::TOOL_OPEN, tool_open) + args + p.atomic_tag(Tag::TOOL_CLOSE, tool_close));
            });

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_call_open = p.choice({p.literal("<tool_call>"), p.literal("<toolcall>")}) + skip_blank_lines;
            auto tool_call_close = p.choice({p.literal("</tool_call>"), p.literal("</toolcall>")});
            auto tool_call = p.rule("tool-call",
                tool_call_open
                + tool_choice
                + tool_call_close
                + skip_blank_lines);
            auto tool_calls = p.trigger_rule("tool-call-root", p.repeat(tool_call, /* min = */ min_calls, /* max = */ max_calls));

            auto content_before = p.optional(p.tag(Tag::CONTENT, p.until_one_of({
                "\n<tool_call>", "\r\n<tool_call>", "<tool_call>",
                "\n<toolcall>", "\r\n<toolcall>", "<toolcall>"
            })));
            auto content_after = p.optional(p.tag(Tag::CONTENT, p.until_one_of({
                "\n<|im_end|>", "\r\n<|im_end|>", "<|im_end|>"
            })));
            auto pre_tool_gap = p.repeat(newline, 0, -1);
            return assistant_prefix + reasoning + after_reasoning_gap + content_before + pre_tool_gap + tool_calls + content_after + assistant_suffix;
        }

        // Content only parser
        include_grammar = false;
        auto content_body = p.optional(p.tag(Tag::CONTENT, p.until_one_of({
            "\n<|im_end|>", "\r\n<|im_end|>", "<|im_end|>"
        })));
        return assistant_prefix + reasoning + after_reasoning_gap + content_body + assistant_suffix;
    });

    data.parser = parser.save();

    if (include_grammar) {
        data.grammar_lazy = has_tools && inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_AUTO;

        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                auto schema = function.at("parameters");
                builder.resolve_refs(schema);
            });
            parser.build_grammar(builder, data.grammar_lazy);
        });

        if (data.grammar_lazy) {
            data.grammar_triggers = {
                {COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<tool_call>"}
            };
        } else {
            data.grammar_triggers.clear();
        }
    }

    return data;
}
 200 changes: 200 additions & 0 deletions200  
common/chat-parsers/qwen3-coder-xml.cpp
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,200 @@
// Qwen3 Coder XML tool call format
// Format: <tool_call><function=name><parameter=key>value</parameter></function></tool_call>

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_qwen3_coder_xml(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_QWEN3_CODER_XML;

    data.preserved_tokens = {
        "<tool_call>",
        "</tool_call>",
        "<function=",
        "</function>",
        "<parameter=",
        "</parameter>",
    };

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto include_grammar = true;

    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;

        const auto consume_end_block = [&]() {
            auto optional_end = p.optional(p.choice({
                p.literal("<|im_end|>"),
                p.literal("<|endoftext|>")
            }));
            return p.optional(p.literal("\n")) + optional_end + p.optional(p.literal("\n"));
        };

        const auto content_until = [&](const std::string & marker, bool allow_inline) {
            std::vector<std::string> delimiters = {
                std::string("\r\n") + marker,
                std::string("\n") + marker,
            };
            if (allow_inline) {
                delimiters.push_back(marker);
            }
            return p.tag(Tag::CONTENT, p.until_one_of(delimiters));
        };

        const auto content_before_tool = p.optional(p.rule("qwen-tool-prefix",
            p.tag(Tag::CONTENT, p.until("<tool_call>"))
            + p.peek(p.literal("<tool_call>"))
        ));

        // Response format parser
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            return p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema))
                << consume_end_block();
        }

        // Tool call parser
        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            auto parameter_name = p.choice();
            parameter_name |= p.tag(Tag::TOOL_ARG_NAME, p.until(">\r\n"));
            parameter_name |= p.tag(Tag::TOOL_ARG_NAME, p.until(">\n"));
            parameter_name |= p.tag(Tag::TOOL_ARG_NAME, p.until(">"));
            auto parameter_terminator = p.choice({
                p.literal(">\r\n"),
                p.literal(">\n"),
                p.literal(">"),
            });

            auto tool_choice = p.choice();
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");

                auto schema_info = common_schema_info();
                schema_info.resolve_refs(parameters);

                bool allow_additional = false;
                bool additional_has_schema = false;
                json additional_schema;
                if (parameters.contains("additionalProperties")) {
                    const auto & additional = parameters.at("additionalProperties");
                    if (additional.is_boolean()) {
                        allow_additional = additional.get<bool>();
                    } else if (additional.is_object()) {
                        allow_additional = true;
                        additional_has_schema = true;
                        additional_schema = additional;
                    }
                }

                auto args = p.sequence();
                foreach_parameter(function, [&](const std::string & param_name, const json & param_schema, bool /* is_required */) {
                    auto parameter_value = p.choice();
                    if (schema_info.resolves_to_string(param_schema)) {
                        parameter_value |= p.tag(Tag::TOOL_ARG_STRING_VALUE, p.until("</parameter>"));
                    } else {
                        parameter_value |= p.tag(Tag::TOOL_ARG_JSON_VALUE,
                            p.schema(p.json(), "qwen-param-" + name + "-" + param_name, param_schema));
                    }

                    auto param_open = p.literal("<parameter=")
                        << p.literal_tag(Tag::TOOL_ARG_NAME, param_name)
                        << parameter_terminator;
                    auto param_close = p.literal("</parameter>");
                    auto arg_rule = p.rule("qwen-parameter-" + name + "-" + param_name,
                        p.atomic_tag(Tag::TOOL_ARG_OPEN, param_open)
                        + parameter_value
                        + p.atomic_tag(Tag::TOOL_ARG_CLOSE, param_close)
                    );

                    args += p.repeat(arg_rule, /* min = */ 0, /* max = */ 1);
                });

                if (allow_additional) {
                    auto additional_value = p.choice();
                    if (additional_has_schema) {
                        if (schema_info.resolves_to_string(additional_schema)) {
                            additional_value |= p.tag(Tag::TOOL_ARG_STRING_VALUE, p.until("</parameter>"));
                        } else {
                            additional_value |= p.tag(Tag::TOOL_ARG_JSON_VALUE,
                                p.schema(p.json(), "qwen-param-" + name + "-additional", additional_schema));
                        }
                    } else {
                        additional_value |= p.tag(Tag::TOOL_ARG_STRING_VALUE, p.until("</parameter>"));
                    }

                    auto generic_open = p.literal("<parameter=")
                        << parameter_name
                        << parameter_terminator;
                    auto generic_close = p.literal("</parameter>");
                    auto additional_rule = p.rule("qwen-parameter-generic-" + name,
                        p.atomic_tag(Tag::TOOL_ARG_OPEN, generic_open)
                        + additional_value
                        + p.atomic_tag(Tag::TOOL_ARG_CLOSE, generic_close)
                    );

                    args += p.repeat(additional_rule, 0, -1);
                }

                // Format: <function=name><parameter=key>value</parameter></function>
                // Allow optional whitespace/indentation for flexibility
                auto tool_open = p.literal("<function=")
                    << p.literal_tag(Tag::TOOL_NAME, name)
                    << p.literal(">");
                auto tool_close = p.literal("</function>");

                tool_choice |= p.rule("tool-" + name,
                    p.atomic_tag(Tag::TOOL_OPEN, tool_open)
                    + args
                    + p.atomic_tag(Tag::TOOL_CLOSE, tool_close)
                );
            });

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_call = p.rule("tool-call",
                p.tag(Tag::TOOL,
                    p.literal("<tool_call>")
                    << tool_choice
                    << p.literal("</tool_call>")
                )
            );
            auto tool_calls = p.trigger_rule("tool-call-root", p.repeat(tool_call, /* min = */ min_calls, /* max = */ max_calls));

            return p.optional(content_before_tool) << tool_calls << consume_end_block();
        }

        // Content only parser
        include_grammar = false;
        return p.choice({
            content_until("<|im_end|>", /* allow_inline = */ true) << consume_end_block(),
            content_until("<|endoftext|>", /* allow_inline = */ true) << consume_end_block(),
            p.tag(Tag::CONTENT, p.rest())
        });
    });

    data.parser = parser.save();

    if (include_grammar) {
        data.grammar_lazy = has_tools && inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_AUTO;

        // Build grammar from PEG parser
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            foreach_function(inputs.tools, [&](const json & tool) {
                auto schema = tool.at("function").at("parameters");
                builder.resolve_refs(schema);
            });
            parser.build_grammar(builder, data.grammar_lazy);
        });

        if (data.grammar_lazy) {
            data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<tool_call>"});
        } else {
            data.grammar_triggers.clear();
        }
    }

    return data;
}
 204 changes: 204 additions & 0 deletions204  
common/chat-parsers/seed-oss.cpp
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,204 @@
// Seed OSS tool call format
// Format: <seed:tool_call><function=name><parameter=key>value</parameter></function></seed:tool_call>
// With optional <seed:think>...</seed:think> reasoning blocks

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_seed_oss(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_SEED_OSS;

    // Handle thinking tags appropriately based on inputs.enable_thinking
    if (string_ends_with(data.prompt, "<seed:think>")) {
        if (!inputs.enable_thinking) {
            data.prompt += "</seed:think>";
        } else {
            data.thinking_forced_open = true;
        }
    }

    data.preserved_tokens = {
        "<seed:think>",
        "</seed:think>",
        "<seed:tool_call>",
        "</seed:tool_call>",
        "<function=",
        "</function>",
        "<parameter=",
        "</parameter>",
        "<seed:eos>",
    };

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;
    auto include_grammar = true;
    bool require_tools = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED;

    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;
        auto newline = p.choice({p.literal("\r\n"), p.literal("\n")});
        auto eos = p.optional(p.repeat(newline, 0, -1) + p.literal("<seed:eos>") + p.repeat(newline, 0, -1));
        auto reasoning = p.eps();
        auto reasoning_block = p.literal("<seed:think>")
            + p.tag(Tag::REASONING, p.until("</seed:think>"))
            + (p.literal("</seed:think>") | p.end());
        if (extract_reasoning) {
            if (inputs.enable_thinking && data.thinking_forced_open) {
                reasoning = reasoning_block;
            } else if (inputs.enable_thinking) {
                reasoning = p.optional(reasoning_block);
            } else {
                reasoning = p.optional(reasoning_block);
            }
        } else {
            reasoning = p.optional(reasoning_block);
        }

        // Response format parser
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            return reasoning << p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema));
        }

        // Tool call parser
        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            auto tool_choice = p.choice();
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");

                auto schema_info = common_schema_info();
                schema_info.resolve_refs(parameters);

                bool allow_additional = false;
                bool additional_has_schema = false;
                json additional_schema;
                if (parameters.contains("additionalProperties")) {
                    const auto & additional = parameters.at("additionalProperties");
                    if (additional.is_boolean()) {
                        allow_additional = additional.get<bool>();
                    } else if (additional.is_object()) {
                        allow_additional = true;
                        additional_has_schema = true;
                        additional_schema = additional;
                    }
                }

                auto tool_open = "<function=" + p.literal_tag(Tag::TOOL_NAME, name) + ">";
                auto tool_close = p.literal("</function>");
                auto args = p.sequence();

                foreach_parameter(function, [&](const auto & param_name, const json & param_schema, bool is_required) {
                    (void) is_required;
                    auto rule_name = "tool-" + name + "-arg-" + param_name;

                    auto arg_open = "<parameter=" + p.literal_tag(Tag::TOOL_ARG_NAME, param_name) + ">";
                    auto arg_close = p.literal("</parameter>");
                    auto arg_value = p.eps();

                    if (schema_info.resolves_to_string(param_schema)) {
                        arg_value = p.tag(Tag::TOOL_ARG_STRING_VALUE, p.until("</parameter>"));
                    } else {
                        arg_value = p.tag(Tag::TOOL_ARG_JSON_VALUE, p.schema(p.json(), rule_name + "-schema", param_schema));
                    }

                    auto arg_rule = p.rule(rule_name,
                        p.atomic_tag(Tag::TOOL_ARG_OPEN, arg_open)
                        + arg_value
                        + p.atomic_tag(Tag::TOOL_ARG_CLOSE, arg_close)
                        + p.space());
                    args += p.repeat(arg_rule, /* min = */ 0, /* max = */ 1);
                });

                if (allow_additional) {
                    auto dynamic_name = p.tag(Tag::TOOL_ARG_NAME, p.until(">"));
                    auto additional_value = p.choice();
                    if (additional_has_schema) {
                        if (schema_info.resolves_to_string(additional_schema)) {
                            additional_value |= p.tag(Tag::TOOL_ARG_STRING_VALUE, p.until("</parameter>"));
                        } else {
                            additional_value |= p.tag(Tag::TOOL_ARG_JSON_VALUE,
                                p.schema(p.json(), "seed-oss-additional-" + name, additional_schema));
                        }
                    } else {
                        additional_value |= p.tag(Tag::TOOL_ARG_STRING_VALUE, p.until("</parameter>"));
                    }

                    auto additional_rule = p.rule("seed-parameter-generic-" + name,
                        p.atomic_tag(Tag::TOOL_ARG_OPEN, "<parameter=" + dynamic_name + ">")
                        + additional_value
                        + p.atomic_tag(Tag::TOOL_ARG_CLOSE, p.literal("</parameter>"))
                        + p.space());
                    args += p.repeat(additional_rule, 0, -1);
                }

                tool_choice |= p.rule("tool-" + name,
                    p.atomic_tag(Tag::TOOL_OPEN, tool_open)
                    << args
                    << p.atomic_tag(Tag::TOOL_CLOSE, tool_close));
            });

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_call = p.rule("tool-call",
                p.literal("<seed:tool_call>")
                << tool_choice
                << p.literal("</seed:tool_call>")
                + p.repeat(newline, 0, -1));
            auto tool_calls = p.trigger_rule("tool-call-root", p.repeat(tool_call, /* min = */ min_calls, /* max = */ max_calls));

            auto stop_before = std::vector<std::string> {
                "\r\n\r\n<seed:tool_call>", "\n\n<seed:tool_call>",
                "\r\n<seed:tool_call>", "\n<seed:tool_call>", "<seed:tool_call>",
                "\r\n\r\n<seed:toolcall>", "\n\n<seed:toolcall>",
                "\r\n<seed:toolcall>", "\n<seed:toolcall>", "<seed:toolcall>",
            };
            auto content_before = p.optional(p.tag(Tag::CONTENT, p.until_one_of(stop_before)));
            auto content_after = p.optional(p.tag(Tag::CONTENT, p.until_one_of({
                "\r\n\r\n<seed:eos>", "\n\n<seed:eos>",
                "\r\n<seed:eos>", "\n<seed:eos>", "<seed:eos>"
            })));
            auto pre_calls_gap = p.repeat(newline, 0, -1);
            if (require_tools) {
                return reasoning << pre_calls_gap << tool_calls << eos;
            }
            return reasoning << content_before << pre_calls_gap << tool_calls << content_after << eos;
        }

        // Content only parser
        include_grammar = false;
        auto content_tail = p.optional(p.tag(Tag::CONTENT, p.until_one_of({
            "\r\n\r\n<seed:eos>", "\n\n<seed:eos>",
            "\r\n<seed:eos>", "\n<seed:eos>", "<seed:eos>"
        })));
        auto pre_eos_gap = p.repeat(newline, 0, -1);
        return reasoning << content_tail << pre_eos_gap << eos;
    });

    data.parser = parser.save();

    if (include_grammar) {
        data.grammar_lazy = has_tools && inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_AUTO;

        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                auto schema = function.at("parameters");
                builder.resolve_refs(schema);
            });
            parser.build_grammar(builder, data.grammar_lazy);
        });

        if (data.grammar_lazy) {
            data.grammar_triggers = {
                {COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<seed:tool_call>"}
            };
        } else {
            data.grammar_triggers.clear();
        }
    }

    return data;
}
 75 changes: 75 additions & 0 deletions75  
common/chat-parsers/xiaomi-mimo.cpp
Original file line number	Diff line number	Diff line change
@@ -0,0 +1,75 @@
// Xiaomi MiMo tool call format
// Format: <tool_call>{"name": "func", "arguments": {...}}</tool_call>

#include "chat-parsers-internal.h"

common_chat_params common_chat_params_init_xiaomi_mimo(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_XIAOMI_MIMO;

    data.preserved_tokens = {
        "<tool_call>",
        "</tool_call>",
    };

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto include_grammar = true;

    bool require_tools = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED;
    auto parser = build_chat_peg_parser([&](auto & p) {
        using Tag = common_chat_peg_tag;

        // Response format parser
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            return p.tag(Tag::CONTENT, p.schema(p.json(), "response-format", inputs.json_schema));
        }

        // Tool call parser
        // Format: <tool_call>{"name": "func", "arguments": {...}}</tool_call>
        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            auto tool_call = p.tag(Tag::TOOL,
                p.token_tag(Tag::TOOL_OPEN, "<tool_call>\n")
                + p.tag(Tag::TOOL_ARGS, p.json())
                + p.token_tag(Tag::TOOL_CLOSE, "\n</tool_call>")
            );

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_calls = p.trigger_rule("tool-call", p.repeat(tool_call, min_calls, max_calls));

            if (require_tools) {
                return tool_calls;
            }
            return p.tag(Tag::CONTENT, p.until("<tool_call>")) << tool_calls;
        }

        // Content only parser
        include_grammar = false;
        return p.tag(Tag::CONTENT, p.rest());
    });

    data.parser = parser.save();

    if (include_grammar) {
        data.grammar_lazy = has_tools && inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_AUTO;

        // Build grammar from PEG parser
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            foreach_function(inputs.tools, [&](const json & tool) {
                auto schema = tool.at("function").at("parameters");
                builder.resolve_refs(schema);
            });
            parser.build_grammar(builder, data.grammar_lazy);
        });

        if (data.grammar_lazy) {
            data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<tool_call>"});
        } else {
            data.grammar_triggers.clear();
        }
    }

    return data;
}
 439 changes: 355 additions & 84 deletions439  
common/chat-peg-parser.cpp
Large diffs are not rendered by default.

  165 changes: 81 additions & 84 deletions165  
common/chat-peg-parser.h
Original file line number	Diff line number	Diff line change
@@ -3,103 +3,100 @@
#include "chat.h"
#include "peg-parser.h"

class common_chat_peg_builder : public common_peg_parser_builder {
  public:
    static constexpr const char * REASONING_BLOCK = "reasoning-block";
    static constexpr const char * REASONING = "reasoning";
    static constexpr const char * CONTENT = "content";

    common_peg_parser reasoning_block(const common_peg_parser & p) { return tag(REASONING_BLOCK, p); }
    common_peg_parser reasoning(const common_peg_parser & p) { return tag(REASONING, p); }
    common_peg_parser content(const common_peg_parser & p) { return tag(CONTENT, p); }
// Chat PEG tag enum - all tags used in chat parsing
enum class common_chat_peg_tag : int {
    NONE = 0,
    // Base tags
    REASONING_BLOCK,
    REASONING,
    CONTENT,
    // Native tool call tags
    TOOL,
    TOOL_OPEN,
    TOOL_CLOSE,
    TOOL_ID,
    TOOL_NAME,
    TOOL_ARGS,
    // Constructed tool call tags
    TOOL_ARG,
    TOOL_ARG_OPEN,
    TOOL_ARG_CLOSE,
    TOOL_ARG_NAME,
    TOOL_ARG_STRING_VALUE,
    TOOL_ARG_JSON_VALUE,
};

inline common_peg_arena build_chat_peg_parser(const std::function<common_peg_parser(common_chat_peg_builder & builder)> & fn) {
    common_chat_peg_builder builder;
    builder.set_root(fn(builder));
    return builder.build();
// Tag to string for debugging/serialization (exhaustive switch)
inline const char * common_chat_peg_tag_to_string(common_chat_peg_tag tag) {
    switch (tag) {
        case common_chat_peg_tag::NONE:                 return "";
        case common_chat_peg_tag::REASONING_BLOCK:      return "reasoning-block";
        case common_chat_peg_tag::REASONING:            return "reasoning";
        case common_chat_peg_tag::CONTENT:              return "content";
        case common_chat_peg_tag::TOOL:                 return "tool";
        case common_chat_peg_tag::TOOL_OPEN:            return "tool-open";
        case common_chat_peg_tag::TOOL_CLOSE:           return "tool-close";
        case common_chat_peg_tag::TOOL_ID:              return "tool-id";
        case common_chat_peg_tag::TOOL_NAME:            return "tool-name";
        case common_chat_peg_tag::TOOL_ARGS:            return "tool-args";
        case common_chat_peg_tag::TOOL_ARG:             return "tool-arg";
        case common_chat_peg_tag::TOOL_ARG_OPEN:        return "tool-arg-open";
        case common_chat_peg_tag::TOOL_ARG_CLOSE:       return "tool-arg-close";
        case common_chat_peg_tag::TOOL_ARG_NAME:        return "tool-arg-name";
        case common_chat_peg_tag::TOOL_ARG_STRING_VALUE: return "tool-arg-string-value";
        case common_chat_peg_tag::TOOL_ARG_JSON_VALUE:  return "tool-arg-json-value";
    }
    return "unknown";
}

class common_chat_peg_mapper {
  public:
    common_chat_msg & result;
// Mapper types: curried functions for AST-to-message conversion
typedef std::function<void(const common_peg_ast_node & node)> common_chat_peg_map_func;
typedef std::function<common_chat_peg_map_func(common_chat_msg & result)> common_chat_peg_mapper;

// Helper to apply a mapper to parse results
inline void apply_chat_peg_mapper(
    const common_chat_peg_mapper & mapper,
    const common_peg_ast_arena & arena,
    const common_peg_parse_result & parse_result,
    common_chat_msg & msg
) {
    auto map_func = mapper(msg);
    arena.visit(parse_result, map_func);
}

    common_chat_peg_mapper(common_chat_msg & msg) : result(msg) {}
// Alias for the tag enum
using Tag = common_chat_peg_tag;

    virtual void from_ast(const common_peg_ast_arena & arena, const common_peg_parse_result & result);
    virtual void map(const common_peg_ast_node & node);
};
// The builder now just inherits from the base - use p.tag(Tag::XXX, parser) directly
using common_chat_peg_builder = common_peg_parser_builder;

class common_chat_peg_native_builder : public common_chat_peg_builder {
  public:
    static constexpr const char * TOOL = "tool";
    static constexpr const char * TOOL_OPEN = "tool-open";
    static constexpr const char * TOOL_CLOSE = "tool-close";
    static constexpr const char * TOOL_ID = "tool-id";
    static constexpr const char * TOOL_NAME = "tool-name";
    static constexpr const char * TOOL_ARGS = "tool-args";

    common_peg_parser tool(const common_peg_parser & p) { return tag(TOOL, p); }
    common_peg_parser tool_open(const common_peg_parser & p) { return atomic(tag(TOOL_OPEN, p)); }
    common_peg_parser tool_close(const common_peg_parser & p) { return atomic(tag(TOOL_CLOSE, p)); }
    common_peg_parser tool_id(const common_peg_parser & p) { return atomic(tag(TOOL_ID, p)); }
    common_peg_parser tool_name(const common_peg_parser & p) { return atomic(tag(TOOL_NAME, p)); }
    common_peg_parser tool_args(const common_peg_parser & p) { return tag(TOOL_ARGS, p); }
};
inline common_peg_arena build_chat_peg_parser(const std::function<common_peg_parser(common_chat_peg_builder & builder)> & fn) {
    common_chat_peg_builder builder;
    builder.set_root(fn(builder));
    return builder.build();
}

class common_chat_peg_native_mapper : public common_chat_peg_mapper {
    common_chat_tool_call * current_tool;
// Base mapper: handles reasoning and content tags
common_chat_peg_mapper common_chat_peg_base_mapper();

  public:
    common_chat_peg_native_mapper(common_chat_msg & msg) : common_chat_peg_mapper(msg) {}
// Native mapper: handles tool calls with pre-parsed JSON args
common_chat_peg_mapper common_chat_peg_native_mapper();

    void map(const common_peg_ast_node & node) override;
};
// Constructed mapper: builds JSON args from individual parsed pieces
common_chat_peg_mapper common_chat_peg_constructed_mapper();

inline common_peg_arena build_chat_peg_native_parser(const std::function<common_peg_parser(common_chat_peg_native_builder & builder)> & fn) {
    common_chat_peg_native_builder builder;
    builder.set_root(fn(builder));
    return builder.build();
}
// FunctionGemma mapper: similar to constructed but uses <escape> delimited strings
common_chat_peg_mapper common_chat_peg_function_gemma_mapper();

class common_chat_peg_constructed_builder : public common_chat_peg_builder {
  public:
    static constexpr const char * TOOL = "tool";
    static constexpr const char * TOOL_OPEN = "tool-open";
    static constexpr const char * TOOL_CLOSE = "tool-close";
    static constexpr const char * TOOL_NAME = "tool-name";
    static constexpr const char * TOOL_ARG = "tool-arg";
    static constexpr const char * TOOL_ARG_OPEN = "tool-arg-open";
    static constexpr const char * TOOL_ARG_CLOSE = "tool-arg-close";
    static constexpr const char * TOOL_ARG_NAME = "tool-arg-name";
    static constexpr const char * TOOL_ARG_STRING_VALUE = "tool-arg-string-value";
    static constexpr const char * TOOL_ARG_JSON_VALUE = "tool-arg-json-value";

    common_peg_parser tool(const common_peg_parser & p) { return tag(TOOL, p); }
    common_peg_parser tool_open(const common_peg_parser & p) { return atomic(tag(TOOL_OPEN, p)); }
    common_peg_parser tool_close(const common_peg_parser & p) { return atomic(tag(TOOL_CLOSE, p)); }
    common_peg_parser tool_name(const common_peg_parser & p) { return atomic(tag(TOOL_NAME, p)); }
    common_peg_parser tool_arg(const common_peg_parser & p) { return tag(TOOL_ARG, p); }
    common_peg_parser tool_arg_open(const common_peg_parser & p) { return atomic(tag(TOOL_ARG_OPEN, p)); }
    common_peg_parser tool_arg_close(const common_peg_parser & p) { return atomic(tag(TOOL_ARG_CLOSE, p)); }
    common_peg_parser tool_arg_name(const common_peg_parser & p) { return atomic(tag(TOOL_ARG_NAME, p)); }
    common_peg_parser tool_arg_string_value(const common_peg_parser & p) { return tag(TOOL_ARG_STRING_VALUE, p); }
    common_peg_parser tool_arg_json_value(const common_peg_parser & p) { return tag(TOOL_ARG_JSON_VALUE, p); }
};
// Short form mapper: handles {"function_name": {"arg1": value1}} format (used by Apertus)
common_chat_peg_mapper common_chat_peg_short_form_mapper();

class common_chat_peg_constructed_mapper : public common_chat_peg_mapper {
    common_chat_tool_call * current_tool;
    int arg_count = 0;
    bool needs_closing_quote = false;
// Generic mapper: handles {"tool_call": {...}}, {"tool_calls": [...]}, or {"response": "..."} format
common_chat_peg_mapper common_chat_peg_generic_mapper();

  public:
    common_chat_peg_constructed_mapper(common_chat_msg & msg) : common_chat_peg_mapper(msg) {}
// OpenAI-style array mapper: handles [{"name": "func", "arguments": {...}, "id": "..."}] format
common_chat_peg_mapper common_chat_peg_oai_array_mapper();

    void map(const common_peg_ast_node & node) override;
};
// Command R7B mapper: handles [{"tool_call_id": "0", "tool_name": "func", "parameters": {...}}] format
common_chat_peg_mapper common_chat_peg_command_r7b_mapper();

inline common_peg_arena build_chat_peg_constructed_parser(const std::function<common_peg_parser(common_chat_peg_constructed_builder & builder)> & fn) {
    common_chat_peg_constructed_builder builder;
    builder.set_root(fn(builder));
    return builder.build();
}
 2,143 changes: 146 additions & 1,997 deletions2,143  
common/chat.cpp
Original file line number	Diff line number	Diff line change
@@ -1,6 +1,7 @@
#include "chat.h"
#include "chat-parser.h"
#include "chat-peg-parser.h"
#include "chat-parsers-internal.h"
#include "common.h"
#include "json-partial.h"
#include "json-schema-to-grammar.h"
@@ -23,15 +24,6 @@

using json = nlohmann::ordered_json;

static std::string format_time(const std::chrono::system_clock::time_point & now, const std::string & format) {
    auto time = std::chrono::system_clock::to_time_t(now);
    auto local_time = *std::localtime(&time);
    std::ostringstream ss;
    ss << std::put_time(&local_time, format.c_str());
    auto res = ss.str();
    return res;
}

static std::string string_diff(const std::string & last, const std::string & current) {
    if (last.empty()) {
        return current;
@@ -145,24 +137,6 @@ struct common_chat_templates {
    std::unique_ptr<common_chat_template> template_tool_use;
};

struct templates_params {
    json messages;
    json tools;
    common_chat_tool_choice tool_choice;
    json json_schema;
    bool parallel_tool_calls;
    common_reasoning_format reasoning_format;
    bool stream;
    std::string grammar;
    bool add_generation_prompt = true;
    bool enable_thinking = true;
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    json extra_context;
    bool add_bos;
    bool add_eos;
    bool is_inference = true;
};

common_chat_tool_choice common_chat_tool_choice_parse_oaicompat(const std::string & tool_choice) {
    if (tool_choice == "auto") {
        return COMMON_CHAT_TOOL_CHOICE_AUTO;
@@ -189,6 +163,14 @@ bool common_chat_templates_support_enable_thinking(const common_chat_templates *
    return rendered_no_thinking.prompt != rendered_with_thinking.prompt;
}

bool common_chat_templates_support_tools(const common_chat_templates * chat_templates) {
    // Check the template that would be used for tools (tool_use variant if available, otherwise default)
    const auto & tmpl = chat_templates->template_tool_use
        ? *chat_templates->template_tool_use
        : *chat_templates->template_default;
    return tmpl.original_caps().supports_tools;
}

template <>
std::vector<common_chat_msg> common_chat_msgs_parse_oaicompat(const json & messages) {
    std::vector<common_chat_msg> msgs;
@@ -648,6 +630,7 @@ const char * common_chat_format_name(common_chat_format format) {
        case COMMON_CHAT_FORMAT_GENERIC: return "Generic";
        case COMMON_CHAT_FORMAT_MISTRAL_NEMO: return "Mistral Nemo";
        case COMMON_CHAT_FORMAT_MAGISTRAL: return "Magistral";
        case COMMON_CHAT_FORMAT_MINISTRAL_3: return "Ministral 3";
        case COMMON_CHAT_FORMAT_LLAMA_3_X: return "Llama 3.x";
        case COMMON_CHAT_FORMAT_LLAMA_3_X_WITH_BUILTIN_TOOLS: return "Llama 3.x with builtin tools";
        case COMMON_CHAT_FORMAT_DEEPSEEK_R1: return "DeepSeek R1";
@@ -661,6 +644,7 @@ const char * common_chat_format_name(common_chat_format format) {
        case COMMON_CHAT_FORMAT_GPT_OSS: return "GPT-OSS";
        case COMMON_CHAT_FORMAT_SEED_OSS: return "Seed-OSS";
        case COMMON_CHAT_FORMAT_NEMOTRON_V2: return "Nemotron V2";
        case COMMON_CHAT_FORMAT_NEMOTRON_V3: return "Nemotron V3";
        case COMMON_CHAT_FORMAT_APERTUS: return "Apertus";
        case COMMON_CHAT_FORMAT_LFM2_WITH_JSON_TOOLS: return "LFM2 with JSON tools";
        case COMMON_CHAT_FORMAT_MINIMAX_M2: return "MiniMax-M2";
@@ -669,9 +653,7 @@ const char * common_chat_format_name(common_chat_format format) {
        case COMMON_CHAT_FORMAT_QWEN3_CODER_XML: return "Qwen3 Coder";
        case COMMON_CHAT_FORMAT_APRIEL_1_5: return "Apriel 1.5";
        case COMMON_CHAT_FORMAT_XIAOMI_MIMO: return "Xiaomi MiMo";
        case COMMON_CHAT_FORMAT_PEG_SIMPLE: return "peg-simple";
        case COMMON_CHAT_FORMAT_PEG_NATIVE: return "peg-native";
        case COMMON_CHAT_FORMAT_PEG_CONSTRUCTED: return "peg-constructed";
        case COMMON_CHAT_FORMAT_FUNCTION_GEMMA: return "FunctionGemma";
        default:
            throw std::runtime_error("Unknown chat format");
    }
@@ -701,203 +683,6 @@ common_reasoning_format common_reasoning_format_from_name(const std::string & fo
    throw std::runtime_error("Unknown reasoning format: " + format);
}

static void foreach_function(const json & tools, const std::function<void(const json &)> & fn) {
    for (const auto & tool : tools) {
        if (!tool.contains("type") || tool.at("type") != "function" || !tool.contains("function")) {
            LOG_INF("Skipping tool without function: %s", tool.dump(2).c_str());
            continue;
        }
        fn(tool);
    }
}

static void foreach_parameter(const json & function, const std::function<void(const std::string &, const json &, bool)> & fn) {
    if (!function.contains("parameters") || !function.at("parameters").is_object()) {
        return;
    }
    const auto & params = function.at("parameters");
    if (!params.contains("properties") || !params.at("properties").is_object()) {
        return;
    }
    const auto & props = params.at("properties");
    std::set<std::string> required;
    if (params.contains("required") && params.at("required").is_array()) {
        params.at("required").get_to(required);
    }
    for (const auto & [name, prop] : props.items()) {
        bool is_required = (required.find(name) != required.end());
        fn(name, prop, is_required);
    }
}

static std::string apply(
    const common_chat_template & tmpl,
    const struct templates_params & inputs,
    const std::optional<json> & messages_override = std::nullopt,
    const std::optional<json> & tools_override = std::nullopt,
    const std::optional<json> & additional_context = std::nullopt)
{
    minja::chat_template_inputs tmpl_inputs;
    tmpl_inputs.messages = messages_override ? *messages_override : inputs.messages;
    if (tools_override) {
        tmpl_inputs.tools = *tools_override;
    } else {
        tmpl_inputs.tools = inputs.tools.empty() ? json() : inputs.tools;
    }
    tmpl_inputs.add_generation_prompt = inputs.add_generation_prompt;
    tmpl_inputs.extra_context = inputs.extra_context;
    tmpl_inputs.extra_context["enable_thinking"] = inputs.enable_thinking;
    if (additional_context) {
        tmpl_inputs.extra_context.merge_patch(*additional_context);
    }
    // TODO: add flag to control date/time, if only for testing purposes.
    // tmpl_inputs.now = std::chrono::system_clock::now();

    minja::chat_template_options tmpl_opts;
    // To avoid double BOS / EOS tokens, we're manually removing begining / trailing tokens
    // instead of using `chat_template_options.use_bos_token = false`, since these tokens
    // may be needed inside the template / between messages too.
    auto result = tmpl.apply(tmpl_inputs, tmpl_opts);
    if (inputs.add_bos && string_starts_with(result, tmpl.bos_token())) {
        result = result.substr(tmpl.bos_token().size());
    }
    if (inputs.add_eos && string_ends_with(result, tmpl.eos_token())) {
        result = result.substr(0, result.size() - tmpl.eos_token().size());
    }
    return result;
}

static common_chat_params common_chat_params_init_generic(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    auto tool_call_schemas = json::array();
    foreach_function(inputs.tools, [&](const json & tool) {
        const auto & function = tool.at("function");
        auto tool_schema = json {
            {"type", "object"},
            {"properties", {
                {"name", {
                    {"type", "string"},
                    {"const", function.at("name")},
                }},
                {"arguments", function.at("parameters")},
            }},
            {"required", json::array({"name", "arguments"})},
        };
        if (function.contains("description")) {
            tool_schema["description"] = function.at("description");
        }
        if (inputs.parallel_tool_calls) {
            tool_schema.at("properties")["id"] = {
                {"type", "string"},
                {"minLength", 4},
            };
            tool_schema.at("required").push_back("id");
        }
        tool_call_schemas.emplace_back(tool_schema);
    });
    const auto tool_call =
        inputs.parallel_tool_calls
            ? json {
                {"type", "object"},
                {"properties", {
                    {"tool_calls", {
                        {"type", "array"},
                        {"items", tool_call_schemas.size() == 1 ? tool_call_schemas[0] : json {
                            {"anyOf", tool_call_schemas},
                        }},
                        {"minItems", 1},
                    }},
                }},
                {"required", json::array({"tool_calls"})},
            }
            : json {
                {"type", "object"},
                {"properties", {
                    {"tool_call", tool_call_schemas.size() == 1 ? tool_call_schemas[0] : json {
                        {"anyOf", tool_call_schemas},
                    }},
                }},
                {"required", json::array({"tool_call"})},
            };
    const auto schema =
        inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED
            ? json {
                {"anyOf", json::array({
                    tool_call,
                    {
                        {"type", "object"},
                        {"properties", {
                            {"response", inputs.json_schema.is_null()
                                ? json {{"type", "string"}}
                                : inputs.json_schema
                            },
                        }},
                        {"required", json::array({"response"})},
                    },
                })}
            }
            : tool_call;

    data.grammar_lazy = false;
    data.grammar = build_grammar([&](const common_grammar_builder & builder) {
        builder.add_schema("root", schema);
    });

    auto tweaked_messages = common_chat_template::add_system(
        inputs.messages,
        "Respond in JSON format, either with `tool_call` (a request to call tools) or with `response` reply to the user's request");

    data.prompt = apply(tmpl, inputs, /* messages_override= */ tweaked_messages);
    data.format = COMMON_CHAT_FORMAT_GENERIC;
    return data;
}

static common_chat_params common_chat_params_init_mistral_nemo(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;
    data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;
    data.grammar = build_grammar([&](const common_grammar_builder & builder) {
        auto schemas = json::array();
        foreach_function(inputs.tools, [&](const json & tool) {
            const auto & function = tool.at("function");
            schemas.push_back({
                {"type", "object"},
                {"properties", {
                    // Important note: the model is probably trained to take a JSON stringified arguments value.
                    // It's hard to constrain that for now (while reusing the JSON schema conversion), so we're just expecting a plain object.
                    {"name", {
                        {"type", "string"},
                        {"const", function.at("name")},
                    }},
                    {"arguments", function.at("parameters")},
                    {"id", {
                        {"type", "string"},
                        // Nemo's template expects a 9-character alphanumeric ID.
                        {"pattern", "^[a-zA-Z0-9]{9}$"},
                    }},
                }},
                {"required", json::array({"name", "arguments", "id"})},
            });
        });
        auto schema = json {
            {"type", "array"},
            {"items", schemas.size() == 1 ? schemas[0] : json {{"anyOf", schemas}}},
            {"minItems", 1},
        };
        if (!inputs.parallel_tool_calls) {
            schema["maxItems"] = 1;
        }
        builder.add_rule("root", "\"[TOOL_CALLS]\" " + builder.add_schema("tool_calls", schema));
    });
    data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "[TOOL_CALLS]"});
    data.preserved_tokens = {
        "[TOOL_CALLS]",
    };
    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_MISTRAL_NEMO;
    return data;
}


// Case-insensitive find
static size_t ifind_string(const std::string & haystack, const std::string & needle, size_t pos = 0) {
@@ -909,1797 +694,168 @@ static size_t ifind_string(const std::string & haystack, const std::string & nee
    return (it == haystack.end()) ? std::string::npos : std::distance(haystack.begin(), it);
}

static common_chat_params common_chat_params_init_lfm2(const common_chat_template & tmpl, const struct templates_params & inputs) {
static common_chat_params common_chat_params_init_without_tools(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;
    const auto is_json_schema_provided = !inputs.json_schema.is_null();
    const auto is_grammar_provided = !inputs.grammar.empty();
    const auto are_tools_provided = inputs.tools.is_array() && !inputs.tools.empty();

    // the logic requires potentially modifying the messages
    auto tweaked_messages = inputs.messages;

    auto replace_json_schema_marker = [](json & messages) -> bool {
        static std::string marker1 = "force json schema.\n";
        static std::string marker2 = "force json schema.";

        if (messages.empty() || messages.at(0).at("role") != "system") {
            return false;
        }

        std::string content = messages.at(0).at("content");

        for (const auto & marker : {marker1, marker2}) {
            const auto pos = ifind_string(content, marker);
            if (pos != std::string::npos) {
                content.replace(pos, marker.length(), "");
                // inject modified content back into the messages
                messages.at(0).at("content") = content;
                return true;
            }
    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_CONTENT_ONLY;
    data.grammar_lazy = false;
    if (!inputs.json_schema.is_null()) {
        if (!inputs.grammar.empty()) {
            throw std::runtime_error("Either \"json_schema\" or \"grammar\" can be specified, but not both");
        }

        return false;
    };

    // Lfm2 model does not natively work with json, but can generally understand the tools structure
    //
    // Example of the pytorch dialog structure:
    //     <|startoftext|><|im_start|>system
    //     List of tools: <|tool_list_start|>[{"name": "get_candidate_status", "description": "Retrieves the current status of a candidate in the recruitment process", "parameters": {"type": "object", "properties": {"candidate_id": {"type": "string", "description": "Unique identifier for the candidate"}}, "required": ["candidate_id"]}}]<|tool_list_end|><|im_end|>
    //     <|im_start|>user
    //     What is the current status of candidate ID 12345?<|im_end|>
    //     <|im_start|>assistant
    //     <|tool_call_start|>[get_candidate_status(candidate_id="12345")]<|tool_call_end|>Checking the current status of candidate ID 12345.<|im_end|>
    //     <|im_start|>tool
    //     <|tool_response_start|>{"candidate_id": "12345", "status": "Interview Scheduled", "position": "Clinical Research Associate", "date": "2023-11-20"}<|tool_response_end|><|im_end|>
    //     <|im_start|>assistant
    //     The candidate with ID 12345 is currently in the "Interview Scheduled" stage for the position of Clinical Research Associate, with an interview date set for 2023-11-20.<|im_end|>
    //
    // For the llama server compatibility with json tools semantic,
    // the client can add "Follow json schema." line into the system message prompt to force the json output.
    //
    if (are_tools_provided && (is_json_schema_provided || is_grammar_provided)) {
        // server/utils.hpp prohibits that branch for the custom grammar anyways
        throw std::runtime_error("Tools call must not use \"json_schema\" or \"grammar\", use non-tool invocation if you want to use custom grammar");
    } else if (are_tools_provided && replace_json_schema_marker(tweaked_messages)) {
        LOG_INF("%s: Using tools to build a grammar\n", __func__);

        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            auto schemas = json::array();
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                schemas.push_back({
                    {"type", "object"},
                    {"properties", {
                        {"name", {
                            {"type", "string"},
                            {"const", function.at("name")},
                        }},
                        {"arguments", function.at("parameters")},
                    }},
                    {"required", json::array({"name", "arguments", "id"})},
                });
            });
            auto schema = json {
                {"type", "array"},
                {"items", schemas.size() == 1 ? schemas[0] : json {{"anyOf", schemas}}},
                {"minItems", 1},
            };
            if (!inputs.parallel_tool_calls) {
                schema["maxItems"] = 1;
            }

            builder.add_rule("root", "\"<|tool_call_start|>\"" + builder.add_schema("tool_calls", schema) + "\"<|tool_call_end|>\"");
        });
        // model has no concept of tool selection mode choice,
        // if the system prompt rendered correctly it will produce a tool call
        // the grammar goes inside the tool call body
        data.grammar_lazy = true;
        data.grammar_triggers = {{COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL, "\\s*<\\|tool_call_start\\|>\\s*\\["}};
        data.preserved_tokens = {"<|tool_call_start|>", "<|tool_call_end|>"};
        data.format = COMMON_CHAT_FORMAT_LFM2_WITH_JSON_TOOLS;
    } else if (are_tools_provided && (!is_json_schema_provided && !is_grammar_provided)) {
        LOG_INF("%s: Using tools without json schema or grammar\n", __func__);
        // output those tokens
        data.preserved_tokens = {"<|tool_call_start|>", "<|tool_call_end|>"};
    } else if (is_json_schema_provided) {
        LOG_INF("%s: Using provided json schema to build a grammar\n", __func__);
        data.grammar = json_schema_to_grammar(inputs.json_schema);
    } else if (is_grammar_provided) {
        LOG_INF("%s: Using provided grammar\n", __func__);
        data.grammar = inputs.grammar;
    } else {
        LOG_INF("%s: Using content relying on the template\n", __func__);
        data.grammar = inputs.grammar;
    }

    data.prompt = apply(tmpl, inputs, /* messages_override= */ tweaked_messages);
    LOG_DBG("%s: Prompt: %s\n", __func__, data.prompt.c_str());

    return data;
}

static common_chat_params common_chat_params_init_ministral_3(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    // Build up messages to follow the format: https://huggingface.co/mistralai/Ministral-3-14B-Reasoning-2512/blob/main/chat_template.jinja
    auto adjusted_messages = json::array();
    for (const auto & msg : inputs.messages) {
        auto role = msg.value("role", "");
        if (role != "system" && role != "assistant") {
            // Only adjust system and assistant messages. Interestingly, the system message may contain thinking.
            adjusted_messages.push_back(msg);
            continue;
        }

        auto content = json::array();

        // If message contains `reasoning_content`, add it as a block of type `thinking`
        if (msg.contains("reasoning_content") && msg.at("reasoning_content").is_string()) {
            content.push_back({
                {"type", "thinking"},
                {"thinking", msg.at("reasoning_content").get<std::string>()},
            });
        }

        // If message contains `content`, add it as a block of type `text`
        if (msg.contains("content")) {
            if (msg.at("content").is_string()) {
                content.push_back({
                    {"type", "text"},
                    {"text", msg.at("content").get<std::string>()},
                });
            } else if (msg.at("content").is_array()) {
                auto blocks = msg.at("content");
                content.insert(content.end(), blocks.begin(), blocks.end());
static common_chat_params common_chat_templates_apply_jinja(
    const struct common_chat_templates        * tmpls,
    const struct common_chat_templates_inputs & inputs)
{
    templates_params params;
    params.tools = common_chat_tools_to_json_oaicompat<json>(inputs.tools);
    const auto & tmpl = params.tools.is_array() && tmpls->template_tool_use
        ? *tmpls->template_tool_use
        : *tmpls->template_default;
    const auto & src = tmpl.source();
    const auto & caps = tmpl.original_caps();
    params.messages = common_chat_msgs_to_json_oaicompat<json>(inputs.messages, /* concat_text= */ !tmpl.original_caps().requires_typed_content);
    if (params.messages.is_array()) {
        for (auto & msg : params.messages) {
            if (!msg.contains("reasoning_content") || msg.at("reasoning_content").is_null()) {
                continue;
            }
            // Some templates (e.g., Apriel 1.5) expect the reasoning text under a 'thought' key.
            if (!msg.contains("thought") || msg.at("thought").is_null()) {
                msg["thought"] = msg.at("reasoning_content");
            }
        }

        auto adjusted = msg;
        adjusted["content"] = content;
        adjusted.erase("reasoning_content");
        adjusted_messages.push_back(adjusted);
    }
    params.add_generation_prompt = inputs.add_generation_prompt;
    params.tool_choice = inputs.tool_choice;
    params.reasoning_format = inputs.reasoning_format;
    params.enable_thinking = inputs.enable_thinking;
    params.grammar = inputs.grammar;
    params.now = inputs.now;
    params.add_bos = tmpls->add_bos;
    params.add_eos = tmpls->add_eos;

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;
    auto include_grammar = true;
    params.extra_context = json::object();
    for (auto el : inputs.chat_template_kwargs) {
        params.extra_context[el.first] = json::parse(el.second);
    }
    if (!params.extra_context.contains("add_thoughts")) {
        params.extra_context["add_thoughts"] = inputs.enable_thinking;
    }

    data.prompt = apply(tmpl, inputs, /* messages_override = */ adjusted_messages);
    data.format = COMMON_CHAT_FORMAT_PEG_NATIVE;
    data.preserved_tokens = {
        "[THINK]",
        "[/THINK]",
        "[TOOL_CALLS]",
        "[ARGS]",
    };
    if (!inputs.json_schema.empty()) {
        params.json_schema = json::parse(inputs.json_schema);
    }

    auto parser = build_chat_peg_native_parser([&](common_chat_peg_native_builder & p) {
        auto reasoning = extract_reasoning ? p.optional("[THINK]" + p.reasoning(p.until("[/THINK]")) + "[/THINK]") : p.eps();
    if (inputs.parallel_tool_calls && !tmpl.original_caps().supports_parallel_tool_calls) {
        LOG_DBG("Disabling parallel_tool_calls because the template does not support it\n");
        params.parallel_tool_calls = false;
    } else {
        params.parallel_tool_calls = inputs.parallel_tool_calls;
    }

        // Response format parser
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            // Ministral wants to emit json surrounded by code fences
            return reasoning << "```json" << p.content(p.schema(p.json(), "response-format", inputs.json_schema)) << "```";
    if (params.tools.is_array()) {
        if (params.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE && !params.grammar.empty()) {
            throw std::runtime_error("Cannot specify grammar with tools");
        }

        // Tool call parser
        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            auto tool_choice = p.choice();
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                const auto & schema = function.at("parameters");

                tool_choice |= p.rule("tool-" + name,
                    p.tool_open(p.tool_name(p.literal(name)) + "[ARGS]")
                    + p.tool_args(p.schema(p.json(), "tool-" + name + "-schema", schema))
                );
            });

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_calls = p.trigger_rule("tool-call", p.repeat("[TOOL_CALLS]" + tool_choice, min_calls, max_calls));

            return reasoning << p.content(p.until("[TOOL_CALLS]")) << tool_calls;
        if (caps.supports_tool_calls && !caps.supports_tools) {
            LOG_WRN("Template supports tool calls but does not natively describe tools. The fallback behaviour used may produce bad results, inspect prompt w/ --verbose & consider overriding the template.\n");
        }
    }

        // Content only parser
        include_grammar = false;
        return reasoning << p.content(p.rest());
    });

    data.parser = parser.save();

    if (include_grammar) {
        data.grammar_lazy = has_tools && inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_AUTO;
    // DeepSeek V3.1: detect based on specific patterns in the template
    if (src.find("message['prefix'] is defined and message['prefix'] and thinking") != std::string::npos &&
        params.json_schema.is_null()) {
        return common_chat_params_init_deepseek_v3_1(tmpl, params);
    }

        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                auto schema = function.at("parameters");
                builder.resolve_refs(schema);
            });
            parser.build_grammar(builder, data.grammar_lazy);
        });
    // DeepSeek R1: use handler in all cases except json schema (thinking / tools).
    if (src.find("<｜tool▁calls▁begin｜>") != std::string::npos && params.json_schema.is_null()) {
        return common_chat_params_init_deepseek_r1(tmpl, params);
    }

        data.grammar_triggers = {
            {COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "[TOOL_CALLS]"}
        };
    // Command R7B: : use handler in all cases except json schema (thinking / tools).
    if (src.find("<|END_THINKING|><|START_ACTION|>") != std::string::npos && params.json_schema.is_null()) {
        return common_chat_params_init_command_r7b(tmpl, params);
    }

    return data;
}
    // Granite (IBM) - detects thinking / tools support
    if (src.find("elif thinking") != std::string::npos && src.find("<|tool_call|>") != std::string::npos) {
        return common_chat_params_init_granite(tmpl, params);
    }

static common_chat_params common_chat_params_init_magistral(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;
    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_MAGISTRAL;
    data.preserved_tokens = {
        "[THINK]",
        "[/THINK]",
    };
    // GLM 4.5: detect by <arg_key> and <arg_value> tags (check before Hermes since both use <tool_call>)
    if (src.find("[gMASK]<sop>") != std::string::npos &&
        src.find("<arg_key>") != std::string::npos &&
        src.find("<arg_value>") != std::string::npos &&
        params.json_schema.is_null()) {
        return common_chat_params_init_glm_4_5(tmpl, params);
    }

    if (inputs.tools.is_array() && !inputs.tools.empty()) {
        data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            auto schemas = json::array();
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                schemas.push_back({
                    {"type", "object"},
                    {"properties", {
                        {"name", {
                            {"type", "string"},
                            {"const", function.at("name")},
                        }},
                        {"arguments", function.at("parameters")},
                        {"id", {
                            {"type", "string"},
                            {"pattern", "^[a-zA-Z0-9]{9}$"},
                        }},
                    }},
                    {"required", json::array({"name", "arguments", "id"})},
                });
            });
            auto schema = json {
                {"type", "array"},
                {"items", schemas.size() == 1 ? schemas[0] : json {{"anyOf", schemas}}},
                {"minItems", 1},
            };
            if (!inputs.parallel_tool_calls) {
                schema["maxItems"] = 1;
            }
            builder.add_rule("root", "\"[TOOL_CALLS]\" " + builder.add_schema("tool_calls", schema));
        });
        data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "[TOOL_CALLS]"});
        data.preserved_tokens.push_back("[TOOL_CALLS]");
    } else {
        data.grammar_lazy = false;
        if (!inputs.json_schema.is_null()) {
            if (!inputs.grammar.empty()) {
                throw std::runtime_error("Either \"json_schema\" or \"grammar\" can be specified, but not both");
            }
            data.grammar = json_schema_to_grammar(inputs.json_schema);
        } else {
            data.grammar = inputs.grammar;
    // Qwen3-Coder XML format detection (must come before Hermes 2 Pro)
    // Detect via explicit XML markers unique to Qwen3-Coder to avoid false positives in other templates.
    // Require presence of <tool_call>, <function=...>, and <parameter=...> blocks.
    if (src.find("<tool_call>") != std::string::npos &&
        src.find("<function>") != std::string::npos &&
        src.find("<function=") != std::string::npos &&
        src.find("<parameters>") != std::string::npos &&
        src.find("<parameter=") != std::string::npos) {
        // Nemotron 3 Nano 30B A3B
        if (src.find("<think>") != std::string::npos) {
            return common_chat_params_init_nemotron_v3(tmpl, params);
        }
        return common_chat_params_init_qwen3_coder_xml(tmpl, params);
    }

    return data;
}
    // Xiaomi MiMo format detection (must come before Hermes 2 Pro)
    if (src.find("<tools>") != std::string::npos &&
        src.find("# Tools") != std::string::npos &&
        src.find("</tools>") != std::string::npos &&
        src.find("<tool_calls>") != std::string::npos &&
        src.find("</tool_calls>") != std::string::npos &&
        src.find("<tool_response>") != std::string::npos) {
        return common_chat_params_init_xiaomi_mimo(tmpl, params);
    }

static common_chat_params common_chat_params_init_command_r7b(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;
    // FunctionGemma format detection
    // Uses <start_function_call>call:name{...}<end_function_call> format
    if (src.find("<start_function_call>") != std::string::npos &&
        src.find("<end_function_call>") != std::string::npos &&
        src.find("<escape>") != std::string::npos) {
        return common_chat_params_init_function_gemma(tmpl, params);
    }

    auto adjusted_messages = json::array();
    for (const auto & msg : inputs.messages) {
        auto has_reasoning_content = msg.contains("reasoning_content") && msg.at("reasoning_content").is_string();
        auto has_tool_calls = msg.contains("tool_calls") && msg.at("tool_calls").is_array();
        if (has_reasoning_content && has_tool_calls) {
            auto adjusted_message = msg;
            adjusted_message["tool_plan"] = msg.at("reasoning_content");
            adjusted_message.erase("reasoning_content");
            adjusted_messages.push_back(adjusted_message);
        } else {
            adjusted_messages.push_back(msg);
        }
    // Apriel 1.5 format detection (must come before Hermes since template contains <tool_call> instructional text)
    if (src.find("<thinking>") != std::string::npos &&
        src.find("</thinking>") != std::string::npos &&
        src.find("<available_tools>") != std::string::npos &&
        src.find("<|assistant|>") != std::string::npos &&
        src.find("<|tool_result|>") != std::string::npos &&
        src.find("<tool_calls>[") != std::string::npos &&
        src.find("]</tool_calls>") != std::string::npos) {
        return common_chat_params_init_apriel_1_5(tmpl, params);
    }
    data.prompt = apply(tmpl, inputs, /* messages_override= */ adjusted_messages);
    data.format = COMMON_CHAT_FORMAT_COMMAND_R7B;
    if (string_ends_with(data.prompt, "<|START_THINKING|>")) {
        if (!inputs.enable_thinking) {
            data.prompt += "<|END_THINKING|>";
        } else {
            data.thinking_forced_open = true;
        }
    } else if (!inputs.enable_thinking && string_ends_with(data.prompt, "<|CHATBOT_TOKEN|>")) {
        data.prompt += "<|START_THINKING|><|END_THINKING|>";
    }

    data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;
    data.grammar = build_grammar([&](const common_grammar_builder & builder) {
        auto schemas = json::array();
        foreach_function(inputs.tools, [&](const json & tool) {
            const auto & function = tool.at("function");
            schemas.push_back({
                {"type", "object"},
                {"properties", {
                    {"tool_call_id", {
                        {"type", "string"},
                        // Command-R's template expects an integer string.
                        {"pattern", "^[0-9]{1,10}$"},
                    }},
                    {"tool_name", {
                        {"type", "string"},
                        {"const", function.at("name")},
                    }},
                    {"parameters", function.at("parameters")},
                }},
                {"required", json::array({"tool_call_id", "tool_name", "parameters"})},
            });
        });
        auto schema = json {
            {"type", "array"},
            {"items", schemas.size() == 1 ? schemas[0] : json {{"anyOf", schemas}}},
            {"minItems", 1},
        };
        if (!inputs.parallel_tool_calls) {
            schema["maxItems"] = 1;
        }
        builder.add_rule("root",
            std::string(data.thinking_forced_open ? "( \"<|END_THINKING|>\" space )? " : "") +
            "\"<|START_ACTION|>\" " + builder.add_schema("tool_calls", schema) + " \"<|END_ACTION|>\"");
    });
    data.grammar_triggers.push_back({
        COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL,
        // If thinking_forced_open, then we capture the </think> tag in the grammar,
        // (important for required tool choice) and in the trigger's first capture (decides what is sent to the grammar)
        std::string(data.thinking_forced_open ? "[\\s\\S]*?(<\\|END_THINKING\\|>\\s*)" : "(?:<\\|START_THINKING\\|>[\\s\\S]*?<\\|END_THINKING\\|>\\s*)?") +
            "(<\\|START_ACTION\\|>)[\\s\\S]*"
    });
    data.preserved_tokens = {
        "<|START_ACTION|>",
        "<|END_ACTION|>",
        "<|START_RESPONSE|>",
        "<|END_RESPONSE|>",
        "<|START_THINKING|>",
        "<|END_THINKING|>",
    };
    return data;
}

static void expect_tool_parameters(const std::string & name, const json & parameters, const std::vector<std::string> & expected_properties) {
    if (!parameters.is_object() || !parameters.contains("type") || parameters.at("type") != "object" || !parameters.contains("properties") || !parameters.contains("required")) {
        throw std::runtime_error("Parameters of tool " + name + " must be an object w/ required properties");
    // Hermes 2/3 Pro, Qwen 2.5 Instruct (w/ tools)
    if (src.find("<tool_call>") != std::string::npos && params.json_schema.is_null()) {
        return common_chat_params_init_hermes_2_pro(tmpl, params);
    }
    const auto & parameters_properties = parameters.at("properties");
    const auto & parameters_required = parameters.at("required");
    for (const auto & prop : expected_properties) {
        if (!parameters_properties.contains(prop)) {
            throw std::runtime_error("Parameters of tool " + name + " is missing property: " + prop); // NOLINT
        }
        if (std::find(parameters_required.begin(), parameters_required.end(), json(prop)) == parameters_required.end()) {
            throw std::runtime_error("Parameters of tool " + name + " must have property marked as required: " + prop); // NOLINT
        }

    // GPT-OSS
    if (src.find("<|channel|>") != std::string::npos) {
        return common_chat_params_init_gpt_oss(tmpl, params);
    }
    if (parameters_properties.size() != expected_properties.size()) {
        throw std::runtime_error("Parameters of tool " + name + " must only have these properties:" + string_join(expected_properties, ", "));
    }
}

static common_chat_params common_chat_params_init_llama_3_x(const common_chat_template & tmpl, const struct templates_params & inputs, bool allow_python_tag_builtin_tools) {
    auto builtin_tools = json::array();
    common_chat_params data;
    if (!inputs.tools.is_null()) {
        data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            std::vector<std::string> tool_rules;

            auto handle_builtin_tool = [&](const std::string & name, const json & parameters) {
                if (name == "wolfram_alpha" || name == "web_search" || name == "brave_search") {
                    // https://github.com/meta-llama/llama-stack/blob/main/llama_stack/providers/remote/tool_runtime/wolfram_alpha/wolfram_alpha.py
                    // https://github.com/meta-llama/llama-stack/blob/main/llama_stack/providers/remote/tool_runtime/brave_search/brave_search.py
                    expect_tool_parameters(name, parameters, {"query"});
                } else if (name == "python" || name == "code_interpreter") {
                    // https://github.com/meta-llama/llama-stack/blob/main/llama_stack/providers/inline/tool_runtime/code_interpreter/code_interpreter.py
                    expect_tool_parameters(name, parameters, {"code"});
                } else {
                    return false;
                }

                std::vector<std::string> kvs;
                for (const auto & [key, value] : parameters.at("properties").items()) {
                    kvs.push_back("\"" + key + "=\" " + builder.add_schema(name + "-args-" + key, value)); // NOLINT
                }

                tool_rules.push_back(
                    builder.add_rule(
                        name + "-call",
                        "\"<|python_tag|>" + name + ".call(\" " + string_join(kvs, " \", \" ") + " \")\""));
                builtin_tools.push_back(name);

                return true;
            };

            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");
                builder.resolve_refs(parameters);

                // https://github.com/meta-llama/llama-stack/tree/main/llama_stack/providers/remote/tool_runtime
                if (allow_python_tag_builtin_tools) {
                    handle_builtin_tool(name, parameters);
                }
                tool_rules.push_back(
                    builder.add_rule(
                        name + "-call",
                        "\"{\" space "
                        "( \"\\\"type\\\"\"       space \":\" space \"\\\"function\\\"\"     space \",\" space )? "
                        "  \"\\\"name\\\"\"       space \":\" space \"\\\"" + name + "\\\"\" space \",\" space "
                        "  \"\\\"parameters\\\"\" space \":\" space " + builder.add_schema(name + "-args", parameters) + " "
                        "\"}\" space"));
            });
            // Small models may hallucinate function names so we match anything (*at the start*) that looks like the JSON of a function call, regardless of the name.
            data.grammar_triggers.push_back({
                COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL,
                "(\\{\\s*(?:\"type\"\\s*:\\s*\"function\"\\s*,\\s*)?\"name\"\\s*:\\s*\")[\\s\\S]*", // + name + "\"[\\s\\S]*",
            });
            if (!builtin_tools.empty()) {
                data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<|python_tag|>"});
                data.preserved_tokens.push_back("<|python_tag|>");
            }
            // Allow a few empty lines on top of the usual constrained json schema space rule.
            builder.add_rule("root", string_join(tool_rules, " | "));
            data.additional_stops.push_back("<|eom_id|>");
        });
        data.format = allow_python_tag_builtin_tools && !builtin_tools.empty()
            ? COMMON_CHAT_FORMAT_LLAMA_3_X_WITH_BUILTIN_TOOLS
            : COMMON_CHAT_FORMAT_LLAMA_3_X;
    } else {
        data.format = COMMON_CHAT_FORMAT_CONTENT_ONLY;
    }
    data.prompt = apply(tmpl, inputs, /* messages_override =*/ std::nullopt, /* tools_override= */ std::nullopt, json {
        {"date_string", format_time(inputs.now, "%d %b %Y")},
        {"tools_in_user_message", false},
        {"builtin_tools", builtin_tools.empty() ? json() : builtin_tools},
    });
    return data;
}

static common_chat_params common_chat_params_init_nemotron_v2(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    // Generate the prompt using the apply() function with the template
    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_NEMOTRON_V2;

    // Handle thinking tags appropriately based on inputs.enable_thinking
    if (string_ends_with(data.prompt, "<think>\n")) {
        if (!inputs.enable_thinking) {
            data.prompt += "</think>";
        } else {
            data.thinking_forced_open = true;
        }
    }

    // When tools are present, build grammar for the <TOOLCALL> format, similar to CommandR, but without tool call ID
    if (!inputs.tools.is_null() && inputs.tools.is_array() && !inputs.tools.empty()) {
        data.grammar_lazy = true;
        data.grammar      = build_grammar([&](const common_grammar_builder & builder) {
            auto schemas = json::array();
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                schemas.push_back({
                    { "type",       "object"                                                   },
                    { "properties",
                        {
                            { "name",
                            {
                                { "type", "string" },
                                { "const", function.at("name") },
                            } },
                            { "arguments", function.at("parameters") },
                        }                                                                        },
                    { "required",   json::array({ "name", "arguments" }) },
                });
            });
            auto schema = json{
                        { "type",     "array"                                                         },
                        { "items",    schemas.size() == 1 ? schemas[0] : json{ { "anyOf", schemas } } },
                        { "minItems", 1                                                               },
            };
            if (!inputs.parallel_tool_calls) {
                schema["maxItems"] = 1;
            }
            builder.add_rule("root",
                                std::string(data.thinking_forced_open ? "( \"</think>\" space )? " : "") +
                                    "\"<TOOLCALL>\" " + builder.add_schema("tool_calls", schema) +
                                    " \"</TOOLCALL>\"");
        });
        data.grammar_triggers.push_back({ COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL,
            // If thinking_forced_open, then we capture the </think> tag in the grammar,
            // (important for required tool choice) and in the trigger's first capture (decides what is sent to the grammar)
            std::string(data.thinking_forced_open ?
                            "[\\s\\S]*?(</think>\\s*)" :
                            "(?:<think>[\\s\\S]*?</think>\\s*)?") +
                "(<TOOLCALL>)[\\s\\S]*" });
    }
    return data;
}

static common_chat_params common_chat_params_init_nemotron_v3(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_PEG_CONSTRUCTED;

    // Handle thinking tags appropriately based on inputs.enable_thinking
    if (string_ends_with(data.prompt, "<think>\n")) {
        if (!inputs.enable_thinking) {
            data.prompt += "</think>";
        } else {
            data.thinking_forced_open = true;
        }
    }

    data.preserved_tokens = {
        "<think>",
        "</think>",
        "<tool_call>",
        "</tool_call>",
    };

    auto has_tools = inputs.tools.is_array() && !inputs.tools.empty();
    auto extract_reasoning = inputs.reasoning_format != COMMON_REASONING_FORMAT_NONE;
    auto include_grammar = true;

    auto parser = build_chat_peg_constructed_parser([&](auto & p) {
        auto reasoning = p.eps();
        if (inputs.enable_thinking && extract_reasoning) {
            auto reasoning_content = p.reasoning(p.until("</think>")) + ("</think>" | p.end());
            if (data.thinking_forced_open) {
                reasoning = reasoning_content;
            }
        }

        // Response format parser
        if (inputs.json_schema.is_object() && !inputs.json_schema.empty()) {
            return reasoning << p.content(p.schema(p.json(), "response-format", inputs.json_schema));
        }

        // Tool call parser
        if (has_tools && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE) {
            auto tool_choice = p.choice();
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");

                auto schema_info = common_schema_info();
                schema_info.resolve_refs(parameters);

                auto tool_open = "<function=" + p.tool_name(p.literal(name)) + ">\n";
                auto tool_close = p.literal("</function>\n");
                auto args = p.sequence();
                auto arg_string = p.rule("xml-arg-string", p.until_one_of({
                    "\n</parameter>",
                    "\n<parameter=",
                    "\n</function>"
                }));

                foreach_parameter(function, [&](const auto & param_name, const json & param_schema, bool is_required) {
                    auto rule_name = "tool-" + name + "-arg-" + param_name;

                    auto arg_open = "<parameter=" + p.tool_arg_name(p.literal(param_name)) + ">\n";
                    auto arg_close = p.literal("</parameter>\n");
                    auto arg_value = p.eps();

                    if (schema_info.resolves_to_string(param_schema)) {
                        arg_value = p.tool_arg_string_value(arg_string) + "\n";
                    } else {
                        arg_value = p.tool_arg_json_value(p.schema(p.json(), rule_name + "-schema", param_schema));
                    }

                    // Model may or my not close with </parameter>
                    auto arg_rule = p.rule(rule_name, p.tool_arg_open(arg_open) + arg_value + p.optional(p.tool_arg_close(arg_close)));
                    args += p.repeat(arg_rule, /* min = */ is_required ? 1 : 0, /* max = */ 1);
                });

                tool_choice |= p.rule("tool-" + name, p.tool_open(tool_open) + args + p.tool_close(tool_close));
            });

            auto min_calls = inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_REQUIRED ? 1 : 0;
            auto max_calls = inputs.parallel_tool_calls ? -1 : 1;
            auto tool_call = p.rule("tool-call", "<tool_call>\n" + tool_choice + "</tool_call>" + p.space());
            auto tool_calls = p.trigger_rule("tool-call-root", p.repeat(tool_call, /* min = */ min_calls, /* max = */ max_calls));

            return reasoning << p.content(p.until("<tool_call>")) << tool_calls;
        }

        // Content only parser
        include_grammar = false;
        return reasoning << p.content(p.rest());
    });

    data.parser = parser.save();

    if (include_grammar) {
        data.grammar_lazy = has_tools && inputs.tool_choice == COMMON_CHAT_TOOL_CHOICE_AUTO;

        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                auto schema = function.at("parameters");
                builder.resolve_refs(schema);
            });
            parser.build_grammar(builder, data.grammar_lazy);
        });

        data.grammar_triggers = {
            {COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<tool_call>"}
        };
    }

    return data;
}


static common_chat_params common_chat_params_init_apertus(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    // Generate the prompt using the apply() function with the template
    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_APERTUS;

    // Handle thinking tags appropriately based on inputs.enable_thinking
    if (string_ends_with(data.prompt, "<|inner_prefix|>")) {
        if (!inputs.enable_thinking) {
            data.prompt += "<|inner_suffix|>";
        } else {
            data.thinking_forced_open = true;
        }
    }

    // When tools are present, build grammar for the <|tools_prefix|> format
    if (!inputs.tools.is_null() && inputs.tools.is_array() && !inputs.tools.empty()) {
        data.grammar_lazy = true;
        data.grammar      = build_grammar([&](const common_grammar_builder & builder) {
            auto schemas = json::array();
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                schemas.push_back({
                    { "type",       "object"                                                   },
                    { "properties",
                        {
                            { function.at("name"), function.at("parameters") }
                        }                                                                        },
                    { "required",   json::array({ function.at("name") }) },
                });
            });
            auto schema = json{
                        { "type",     "array"                                                         },
                        { "items",    schemas.size() == 1 ? schemas[0] : json{ { "anyOf", schemas } } },
                        { "minItems", 1                                                               },
            };
            if (!inputs.parallel_tool_calls) {
                schema["maxItems"] = 1;
            }
            builder.add_rule("root",
                                std::string(data.thinking_forced_open ? "( \"<|inner_suffix|>\" space )? " : "") +
                                    "\"<|tools_prefix|>\"" + builder.add_schema("tool_calls", schema) + "\"<|tools_suffix|>\"");
                            });
        data.grammar_triggers.push_back({ COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL,
            // If thinking_forced_open, then we capture the <|inner_suffix|> tag in the grammar,
            // (important for required tool choice) and in the trigger's first capture (decides what is sent to the grammar)
            std::string(data.thinking_forced_open ?
                            "[\\s\\S]*?(<\\|inner_suffix\\|>\\s*)" :
                            "(?:<\\|inner_prefix\\|>[\\s\\S]*?<\\|inner_suffix\\|>\\s*)?") +
                "(<\\|tools_prefix\\|>)[\\s\\S]*" });
        data.preserved_tokens = {
            "<|system_start|>",
            "<|system_end|>",
            "<|developer_start|>",
            "<|developer_end|>",
            "<|user_start|>",
            "<|user_end|>",
            "<|assistant_start|>",
            "<|assistant_end|>",
            "<|inner_prefix|>",
            "<|inner_suffix|>",
            "<|tools_prefix|>",
            "<|tools_suffix|>",
        };
    }
    return data;
}

static common_chat_params common_chat_params_init_deepseek_r1(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;
    auto prompt = apply(tmpl, inputs);

    // Hacks to fix the official (broken) prompt.
    // It is advisable to use --chat-template-file models/templates/llama-cpp-deepseek-r1.jinja instead,
    // until the official template is fixed.
    if (tmpl.source().find("{% if ns.is_tool %}{{'<｜tool▁outputs▁end｜>'}}") != std::string::npos) {
        // Don't leave the chat dangling after tool results
        if (string_ends_with(prompt, "<｜tool▁outputs▁end｜>")) {
            prompt += "<｜end▁of▁sentence｜>";
            if (inputs.add_generation_prompt) {
                prompt += "<｜Assistant｜>";
            }
        }
        // Fix up tool call delta example added by Minja
        prompt = std::regex_replace(
            prompt,
            std::regex("(<｜tool▁call▁end｜>)[\\s\\r\\n]*(<｜tool▁outputs▁begin｜>|<｜User｜>)"),
            "$1<｜tool▁calls▁end｜><｜end▁of▁sentence｜>$2");
    }
    data.prompt = prompt;
    data.format = COMMON_CHAT_FORMAT_DEEPSEEK_R1;
    if (string_ends_with(data.prompt, "<think>\n")) {
        if (!inputs.enable_thinking) {
            data.prompt += "</think>";
        } else {
            data.thinking_forced_open = true;
        }
    }

    if (inputs.tools.is_array() && !inputs.tools.empty()) {
        data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED && inputs.json_schema.is_null();
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            std::vector<std::string> tool_rules;
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");
                builder.resolve_refs(parameters);
                tool_rules.push_back(builder.add_rule(name + "-call",
                    "( \"<｜tool▁call▁begin｜>\" )? \"function<｜tool▁sep｜>" + name + "\\n"
                    "```json\\n\" " + builder.add_schema(name + "-args", parameters) + " "
                    "\"```<｜tool▁call▁end｜>\""));
            });
            // Distill Qwen 7B & 32B models seem confused re/ syntax of their tool call opening tag,
            // so we accept common variants (then it's all constrained)
            builder.add_rule("root",
                std::string(data.thinking_forced_open ? "( \"</think>\" space )? " : "") +
                "( \"<｜tool▁calls▁begin｜>\" | \"<｜tool_calls_begin｜>\" | \"<｜tool calls begin｜>\" | \"<｜tool\\\\_calls\\\\_begin｜>\" | \"<｜tool▁calls｜>\" ) "
                "(" + string_join(tool_rules, " | ") + ")" + (inputs.parallel_tool_calls ? "*" : "") + " "
                "\"<｜tool▁calls▁end｜>\""
                " space");
            data.grammar_triggers.push_back({
                COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL,
                // If thinking_forced_open, then we capture the </think> tag in the grammar,
                // (important for required tool choice) and in the trigger's first capture (decides what is sent to the grammar)
                std::string(data.thinking_forced_open ? "[\\s\\S]*?(</think>\\s*)" : "(?:<think>[\\s\\S]*?</think>\\s*)?") +
                    "(<｜tool▁calls▁begin｜>|<｜tool_calls_begin｜>|<｜tool calls begin｜>|<｜tool\\\\_calls\\\\_begin｜>|<｜tool▁calls｜>)[\\s\\S]*"
            });
            data.preserved_tokens = {
                "<think>",
                "</think>",
                "<｜tool▁calls▁begin｜>",
                "<｜tool▁call▁begin｜>",
                "<｜tool▁sep｜>",
                "<｜tool▁call▁end｜>",
                "<｜tool▁calls▁end｜",
            };
        });
    }
    return data;
}

static common_chat_params common_chat_params_init_deepseek_v3_1(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    // Pass thinking context for DeepSeek V3.1 template
    json additional_context = {
        {"thinking", inputs.enable_thinking},
    };

    auto prompt = apply(tmpl, inputs,
                       /* messages_override= */ inputs.messages,
                       /* tools_override= */ std::nullopt,
                       additional_context);
    data.prompt = prompt;
    data.format = COMMON_CHAT_FORMAT_DEEPSEEK_V3_1;
    if (string_ends_with(data.prompt, "<think>")) {
        if (!inputs.enable_thinking) {
            data.prompt += "</think>";
        } else {
            data.thinking_forced_open = true;
        }
    }
    if (inputs.tools.is_array() && !inputs.tools.empty()) {
        data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED && inputs.json_schema.is_null();
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            std::vector<std::string> tool_rules;
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");
                builder.resolve_refs(parameters);
                tool_rules.push_back(builder.add_rule(name + "-call",
                    "( \"<｜tool▁call▁begin｜>\" )? \"" + name + "<｜tool▁sep｜>"
                    "\" " + builder.add_schema(name + "-args", parameters) + " "
                    "\"<｜tool▁call▁end｜>\""));
            });
            // Distill Qwen 7B & 32B models seem confused re/ syntax of their tool call opening tag,
            // so we accept common variants (then it's all constrained)
            builder.add_rule("root",
                std::string(data.thinking_forced_open ? "( \"</think>\" space )? " : "") +
                "( \"<｜tool▁calls▁begin｜>\" | \"<｜tool_calls_begin｜>\" | \"<｜tool calls begin｜>\" | \"<｜tool\\\\_calls\\\\_begin｜>\" | \"<｜tool▁calls｜>\" ) "
                "(" + string_join(tool_rules, " | ") + ")" + (inputs.parallel_tool_calls ? "*" : "") + " "
                "\"<｜tool▁calls▁end｜>\""
                " space");
            data.grammar_triggers.push_back({
                COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL,
                // If thinking_forced_open, then we capture the </think> tag in the grammar,
                // (important for required tool choice) and in the trigger's first capture (decides what is sent to the grammar)
                std::string(data.thinking_forced_open ? "[\\s\\S]*?(</think>\\s*)" : "(?:<think>[\\s\\S]*?</think>\\s*)?") +
                    "(<｜tool▁calls▁begin｜>|<｜tool_calls_begin｜>|<｜tool calls begin｜>|<｜tool\\\\_calls\\\\_begin｜>|<｜tool▁calls｜>)[\\s\\S]*"
            });
            data.preserved_tokens = {
                "<think>",
                "</think>",
                "<｜tool▁calls▁begin｜>",
                "<｜tool▁call▁begin｜>",
                "<｜tool▁sep｜>",
                "<｜tool▁call▁end｜>",
                "<｜tool▁calls▁end｜>",
            };
        });
    }
    return data;
}

static common_chat_params common_chat_params_init_minimax_m2(const common_chat_template & tmpl, const struct templates_params & params) {
    common_chat_params data;
    data.grammar_lazy = params.tools.is_array() && !params.tools.empty() && params.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;

    data.prompt = apply(tmpl, params);
    data.format = COMMON_CHAT_FORMAT_MINIMAX_M2;

    // Handle thinking tags based on prompt ending
    if (string_ends_with(data.prompt, "<think>\n")) {
        if (!params.enable_thinking) {
            // Close the thinking tag immediately if thinking is disabled
            data.prompt += "</think>\n\n";
        } else {
            // Mark thinking as forced open (template started with <think>)
            data.thinking_forced_open = true;
        }
    }

    // Preserve MiniMax-M2 special tokens
    data.preserved_tokens = {
        "<think>",
        "</think>",
        "<minimax:tool_call>",
        "</minimax:tool_call>",
    };

    // build grammar for tool call
    static const xml_tool_call_format form {
        /* form.scope_start = */ "<minimax:tool_call>\n",
        /* form.tool_start  = */ "<invoke name=\"",
        /* form.tool_sep    = */ "\">\n",
        /* form.key_start   = */ "<parameter name=\"",
        /* form.key_val_sep = */ "\">",
        /* form.val_end     = */ "</parameter>\n",
        /* form.tool_end    = */ "</invoke>\n",
        /* form.scope_end   = */ "</minimax:tool_call>",
    };
    build_grammar_xml_tool_call(data, params.tools, form);

    return data;
}

static common_chat_params common_chat_params_init_qwen3_coder_xml(const common_chat_template & tmpl, const struct templates_params & params) {
    common_chat_params data;
    data.grammar_lazy = params.tools.is_array() && !params.tools.empty() && params.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;

    data.prompt = apply(tmpl, params);
    data.format = COMMON_CHAT_FORMAT_QWEN3_CODER_XML;

    data.preserved_tokens = {
        "<tool_call>",
        "</tool_call>",
        "<function=",
        "</function>",
        "<parameter=",
        "</parameter>",
    };

    // build grammar for tool call
    static const xml_tool_call_format form {
        /* form.scope_start = */ "<tool_call>\n",
        /* form.tool_start  = */ "<function=",
        /* form.tool_sep    = */ ">\n",
        /* form.key_start   = */ "<parameter=",
        /* form.key_val_sep = */ ">\n",
        /* form.val_end     = */ "\n</parameter>\n",
        /* form.tool_end    = */ "</function>\n",
        /* form.scope_end   = */ "</tool_call>",
    };
    build_grammar_xml_tool_call(data, params.tools, form);

    return data;
}

static common_chat_params common_chat_params_init_kimi_k2(const common_chat_template & tmpl, const struct templates_params & params) {
    common_chat_params data;
    data.grammar_lazy = params.tools.is_array() && !params.tools.empty() && params.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;

    data.prompt = apply(tmpl, params);
    data.format = COMMON_CHAT_FORMAT_KIMI_K2;

    data.preserved_tokens = {
        "<think>",
        "</think>",
        "<|tool_calls_section_begin|>",
        "<|tool_call_begin|>",
        "<|tool_call_argument_begin|>",
        "<|tool_call_end|>",
        "<|tool_calls_section_end|>",
        "<|im_end|>",
        "<|im_system|>",
        "<|im_middle|>",
    };

    data.additional_stops.insert(data.additional_stops.end(), {
        "<|im_end|>",
        "<|im_middle|>"
    });
    // build grammar for tool call
    static const xml_tool_call_format form = ([]() {
        xml_tool_call_format form {};
        form.scope_start = "<|tool_calls_section_begin|>";
        form.tool_start  = "<|tool_call_begin|>";
        form.tool_sep    = "<|tool_call_argument_begin|>{";
        form.key_start   = "\"";
        form.key_val_sep = "\": ";
        form.val_end     = ", ";
        form.tool_end    = "}<|tool_call_end|>";
        form.scope_end   = "<|tool_calls_section_end|>";
        form.raw_argval  = false;
        form.last_val_end = "";
        return form;
    })();
    build_grammar_xml_tool_call(data, params.tools, form);

    return data;
}

static common_chat_params common_chat_params_init_apriel_1_5(const common_chat_template & tmpl, const struct templates_params & params) {
    common_chat_params data;
    data.grammar_lazy = params.tools.is_array() && !params.tools.empty() && params.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;

    data.prompt = apply(tmpl, params);
    data.format = COMMON_CHAT_FORMAT_APRIEL_1_5;

    data.preserved_tokens = {
        "<thinking>",
        "</thinking>",
        "<tool_calls>",
        "</tool_calls>",
    };

    // build grammar for tool call
    static const xml_tool_call_format form = ([]() {
        xml_tool_call_format form {};
        form.scope_start = "<tool_calls>[";
        form.tool_start  = "{\"name\": \"";
        form.tool_sep    = "\", \"arguments\": {";
        form.key_start   = "\"";
        form.key_val_sep = "\": ";
        form.val_end     = ", ";
        form.tool_end    = "}, ";
        form.scope_end   = "]</tool_calls>";
        form.raw_argval  = false;
        form.last_val_end = "";
        form.last_tool_end = "}";
        return form;
    })();
    build_grammar_xml_tool_call(data, params.tools, form);

    return data;
}

static common_chat_params common_chat_params_init_xiaomi_mimo(const common_chat_template & tmpl, const struct templates_params & params) {
    common_chat_params data;
    data.grammar_lazy = params.tools.is_array() && !params.tools.empty() && params.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;

    data.prompt = apply(tmpl, params);
    data.format = COMMON_CHAT_FORMAT_XIAOMI_MIMO;

    data.preserved_tokens = {
        "<tool_call>",
        "</tool_call>",
    };

    // build grammar for tool call
    static const xml_tool_call_format form = ([]() {
        xml_tool_call_format form {};
        form.scope_start = "\n";
        form.tool_start  = "<tool_call>\n{\"name\": \"";
        form.tool_sep    = "\", \"arguments\": {";
        form.key_start   = "\"";
        form.key_val_sep = "\": ";
        form.val_end     = ", ";
        form.tool_end    = "}\n</tool_call>";
        form.scope_end   = "";
        form.raw_argval  = false;
        form.last_val_end = "";
        return form;
    })();
    build_grammar_xml_tool_call(data, params.tools, form);

    return data;
}

static common_chat_params common_chat_params_init_gpt_oss(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    // Copy reasoning to the "thinking" field as expected by the gpt-oss template
    auto adjusted_messages = json::array();
    for (const auto & msg : inputs.messages) {
        auto has_reasoning_content = msg.contains("reasoning_content") && msg.at("reasoning_content").is_string();
        auto has_tool_calls = msg.contains("tool_calls") && msg.at("tool_calls").is_array();

        if (has_reasoning_content && has_tool_calls) {
            auto adjusted_message = msg;
            adjusted_message["thinking"] = msg.at("reasoning_content");
            adjusted_messages.push_back(adjusted_message);
        } else {
            adjusted_messages.push_back(msg);
        }
    }

    auto prompt = apply(tmpl, inputs, /* messages_override= */ adjusted_messages);

    // Check if we need to replace the return token with end token during
    // inference and without generation prompt. For more details see:
    // https://github.com/ggml-org/llama.cpp/issues/15417
    if (inputs.is_inference && !inputs.add_generation_prompt) {
        static constexpr std::string_view return_token = "<|return|>";
        static constexpr std::string_view end_token    = "<|end|>";
        if (size_t pos = prompt.rfind(return_token); pos != std::string::npos) {
            prompt.replace(pos, return_token.length(), end_token);
        }
    }

    data.prompt = prompt;
    data.format = COMMON_CHAT_FORMAT_GPT_OSS;

    // These special tokens are required to parse properly, so we include them
    // even if parse_tool_calls is false.
    data.preserved_tokens = {
        "<|channel|>",
        "<|constrain|>",
        "<|message|>",
        "<|start|>",
        "<|end|>",
    };

    if (!inputs.json_schema.is_null()) {
        data.grammar_lazy = false;
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            auto schema = inputs.json_schema;
            builder.resolve_refs(schema);

            auto not_end = builder.add_rule("not-end",
                "[^<] | \"<\" [^|] | \"<|\" [^e] | \"<|e\" [^n] | \"<|en\" [^d] | \"<|end\" [^|] | \"<|end|\" [^>]");
            auto analysis = builder.add_rule("analysis",
                "\"<|channel|>analysis<|message|>\" ( " + not_end + " )* \"<|end|>\"");
            auto constraint = builder.add_rule("constraint", "\"<|constrain|>\"? [a-zA-Z0-9_-]+");
            auto final = builder.add_rule("final",
                "\"<|channel|>final\" ( \" \" " + constraint + " )? \"<|message|>\" " +
                builder.add_schema("response", schema)
            );

            builder.add_rule("root", "( " + analysis + " \"<|start|>assistant\" )? " + final);
        });
    }

    if (inputs.tools.is_array() && !inputs.tools.empty()) {
        data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            // tool calls can appear in commentary or analysis channels
            auto channel = builder.add_rule("channel", "\"<|channel|>\" ( \"commentary\" | \"analysis\" )");

            std::vector<std::string> tool_rules_recipient_in_role;
            std::vector<std::string> tool_rules_recipient_in_channel;
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");
                builder.resolve_refs(parameters);

                tool_rules_recipient_in_role.push_back(
                    builder.add_rule(name + "-call",
                        "\"" + name + "\"" + channel + " \" <|constrain|>json\"? \"<|message|>\" " +
                        builder.add_schema(name + "-args", parameters)
                    )
                );

                tool_rules_recipient_in_channel.push_back(
                    builder.add_rule(name + "-call",
                        "\"" + name + "\"" + " \" <|constrain|>json\"? \"<|message|>\" " +
                        builder.add_schema(name + "-args", parameters)
                    )
                );
            });

            auto recipient_in_channel = builder.add_rule("recipient_in_channel",
                channel + " \" to=functions.\" ( " +
                string_join(tool_rules_recipient_in_channel, " | ") + " )"
            );

            if (data.grammar_lazy) {
                auto recipient_in_role = builder.add_rule("recipient_in_role",
                    "\"<|start|>assistant\"? \" to=functions.\" ( " +
                    string_join(tool_rules_recipient_in_role, " | ") + " )"
                );

                builder.add_rule("root", recipient_in_role + " | " + recipient_in_channel);
            } else {
                auto not_end = builder.add_rule("not-end",
                    "[^<] | \"<\" [^|] | \"<|\" [^e] | \"<|e\" [^n] | \"<|en\" [^d] | \"<|end\" [^|] | \"<|end|\" [^>]");
                auto analysis = builder.add_rule("analysis",
                    "\"<|channel|>analysis<|message|>\" ( " + not_end + " )* \"<|end|>\"");
                auto commentary = builder.add_rule("commentary",
                    "\"<|channel|>commentary<|message|>\" ( " + not_end + " )* \"<|end|>\"");

                auto recipient_in_role = builder.add_rule("recipient_in_role",
                    "\" to=functions.\" ( " + string_join(tool_rules_recipient_in_role, " | ") + " )"
                );

                builder.add_rule("root",
                    "( " + analysis + " \"<|start|>assistant\" )? " +
                    "( " + commentary + " \"<|start|>assistant\" )? " +
                    "( " + recipient_in_role + " | " + recipient_in_channel + " )"
                );
            }

            // Trigger on tool calls that appear in the commentary channel
            data.grammar_triggers.push_back({
                COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN,
                "<\\|channel\\|>(commentary|analysis) to"
            });

            // Trigger tool calls that appear in the role section, either at the
            // start or in the middle.
            data.grammar_triggers.push_back({
                COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL,
                "^ to"
            });

            data.grammar_triggers.push_back({
                COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN,
                "<\\|start\\|>assistant to"
            });
        });
    }

    return data;
}

static common_chat_params common_chat_params_init_glm_4_5(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;
    data.grammar_lazy = inputs.tools.is_array() && !inputs.tools.empty() && inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;

    std::string prompt = apply(tmpl, inputs);

    // match the existing trimming behavior
    if (inputs.add_bos && string_starts_with(prompt, tmpl.bos_token())) {
        prompt.erase(0, tmpl.bos_token().size());
    }
    if (inputs.add_eos && string_ends_with(prompt, tmpl.eos_token())) {
        prompt.erase(prompt.size() - tmpl.eos_token().size());
    }
    if (string_ends_with(prompt, "<think>")) {
        if (!inputs.enable_thinking) {
            prompt += "</think>";
        } else {
            data.thinking_forced_open = true;
        }
    }

    // add GLM preserved tokens
    data.preserved_tokens = {
        "<|endoftext|>",
        "[MASK]",
        "[gMASK]",
        "[sMASK]",
        "<sop>",
        "<eop>",
        "<|system|>",
        "<|user|>",
        "<|assistant|>",
        "<|observation|>",
        "<|begin_of_image|>",
        "<|end_of_image|>",
        "<|begin_of_video|>",
        "<|end_of_video|>",
        "<|begin_of_audio|>",
        "<|end_of_audio|>",
        "<|begin_of_transcription|>",
        "<|end_of_transcription|>",
        "<|code_prefix|>",
        "<|code_middle|>",
        "<|code_suffix|>",
        "/nothink",
        "<think>",
        "</think>",
        "<tool_call>",
        "</tool_call>",
        "<arg_key>",
        "</arg_key>",
        "<arg_value>",
        "</arg_value>"
    };

    // extra GLM 4.5 stop word
    data.additional_stops.insert(data.additional_stops.end(), {
        "<|user|>",
        "<|observation|>"
    });

    // build grammar for tool call
    static const xml_tool_call_format form {
        /* form.scope_start = */ "",
        /* form.tool_start  = */ "\n<tool_call>",
        /* form.tool_sep    = */ "\n",
        /* form.key_start   = */ "<arg_key>",
        /* form.key_val_sep = */ "</arg_key>\n<arg_value>",
        /* form.val_end     = */ "</arg_value>\n",
        /* form.tool_end    = */ "</tool_call>\n",
        /* form.scope_end   = */ "",
    };
    build_grammar_xml_tool_call(data, inputs.tools, form);

    data.prompt = prompt;
    data.format = COMMON_CHAT_FORMAT_GLM_4_5;
    return data;
}

static common_chat_params common_chat_params_init_firefunction_v2(const common_chat_template & tmpl, const struct templates_params & inputs) {
    LOG_DBG("%s\n", __func__);
    common_chat_params data;
    const std::optional<json> tools_override = json();
    const std::optional<json> additional_context = json {
        {"datetime", format_time(inputs.now, "%b %d %Y %H:%M:%S GMT")},
        {"functions", json(inputs.tools.empty() ? "" : inputs.tools.dump(2))},
    };
    data.prompt = apply(tmpl, inputs, /* messages_override =*/ std::nullopt, tools_override, additional_context);
    if (inputs.tools.is_array() && !inputs.tools.empty()) {
        data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            auto schemas = json::array();
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                schemas.push_back({
                    {"type", "object"},
                    {"properties", {
                        {"name", {
                            {"type", "string"},
                            {"const", function.at("name")},
                        }},
                        {"arguments", function.at("parameters")},
                    }},
                    {"required", json::array({"name", "arguments", "id"})},
                });
            });
            auto schema = json {
                {"type", "array"},
                {"items", schemas.size() == 1 ? schemas[0] : json {{"anyOf", schemas}}},
                {"minItems", 1},
            };
            if (!inputs.parallel_tool_calls) {
                schema["maxItems"] = 1;
            }
            builder.add_rule("root", "\" functools\"? " + builder.add_schema("tool_calls", schema));
        });
        data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, " functools["});
        data.preserved_tokens = {
            " functools[",
        };
        data.format = COMMON_CHAT_FORMAT_FIREFUNCTION_V2;
    } else {
        data.format = COMMON_CHAT_FORMAT_CONTENT_ONLY;
    }
    return data;
}

static common_chat_params common_chat_params_init_functionary_v3_2(const common_chat_template & tmpl, const struct templates_params & inputs) {
    // >>>all\nlet's call functions>>>fn1\n{"arg1": 1...}\n>>>fn2\n{"arg1": 1...}...
    // Using ">>>f1\n", ">>>f2\n"... as trigger words for the grammar
    // If the function is python, we also allow raw python code (if the line after `python\n` doesn't start w/ opening `{`), which the model seems to prefer for multiline code.
    common_chat_params data;
    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_FUNCTIONARY_V3_2;
    if (inputs.tools.is_array() && !inputs.tools.empty()) {
        data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            std::vector<std::string> first_tool_rules;
            std::vector<std::string> subsequent_tool_rules;
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");
                builder.resolve_refs(parameters);
                std::string args_pattern = "[\\s\\S]*";
                auto args_rule = builder.add_schema(name + "-args", parameters);
                if (name == "python") {
                    args_rule = builder.add_rule(name + "-maybe-raw-args", args_rule + " | [^{] .*");
                } else {
                    args_pattern = "\\{" + args_pattern;
                }
                auto call_rule = builder.add_rule(name + "-call", "\"" + name + "\\n\" " + args_rule);
                first_tool_rules.push_back(call_rule);
                if (inputs.parallel_tool_calls) {
                    subsequent_tool_rules.push_back(builder.add_rule(name + "-call2", "\">>>\" " + call_rule));
                }
                data.grammar_triggers.push_back({
                    COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL,
                    "((?:[\\s\\S]+?>>>)?" + regex_escape(name) + "\n)" + args_pattern,
                });
            });
            data.preserved_tokens = {
                "<|end_header_id|>",
            };
            auto first_rule = first_tool_rules.empty() ? "" : builder.add_rule("first_tool_call", string_join(first_tool_rules, " | ")) + " space";
            if (inputs.parallel_tool_calls) {
                auto subsequent_rule = builder.add_rule("subsequent_tool_call", string_join(subsequent_tool_rules, " | ")) + " space";
                builder.add_rule("root", first_rule + " (" + subsequent_rule + ")*");
            } else {
                builder.add_rule("root", first_rule);
            }

        });
    }
    return data;
}

static common_chat_params common_chat_params_init_functionary_v3_1_llama_3_1(const common_chat_template & tmpl, const struct templates_params & inputs) {
    // https://github.com/MeetKai/functionary/blob/main/tests/prompt_test_v3-llama3.1.txt
    common_chat_params data;

    if (!inputs.tools.is_null()) {
        std::string python_code_argument_name;
        auto has_raw_python = false;

        data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            std::vector<std::string> tool_rules;
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                const auto & parameters = function.at("parameters");
                std::string name = function.at("name");
                if (name == "python" || name == "ipython") {
                    if (!parameters.contains("type")) {
                        throw std::runtime_error("Missing type in python tool");
                    }
                    has_raw_python = true;
                    const auto & type = parameters.at("type");
                    if (type == "object") {
                        auto properties = parameters.at("properties");
                        for (auto it = properties.begin(); it != properties.end(); ++it) {
                            if (it.value().at("type") == "string") {
                                if (!python_code_argument_name.empty()) {
                                    throw std::runtime_error("Multiple string arguments found in python tool");
                                }
                                python_code_argument_name = it.key();
                            }
                        }
                        if (python_code_argument_name.empty()) {
                            throw std::runtime_error("No string argument found in python tool");
                        }
                    } else if (type != "string") {
                        throw std::runtime_error("Invalid type in python tool: " + type.dump());
                    }
                }
                tool_rules.push_back(builder.add_rule(name + "-call", "\"<function=" + name + ">\" " + builder.add_schema(name + "-args", parameters) + " \"</function>\" space"));
            });
            if (has_raw_python) {
                tool_rules.push_back(builder.add_rule("python-call", "\"<|python_tag|>\" .*"));
                data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<|python_tag|>"});
                data.preserved_tokens.push_back("<|python_tag|>");
            }
            auto tool_call = builder.add_rule("tool_call", string_join(tool_rules, " | ")) + " space";
            builder.add_rule("root", inputs.parallel_tool_calls ? "(" + tool_call + ")+" : tool_call);
            data.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<function="});
        });
        data.format = COMMON_CHAT_FORMAT_FUNCTIONARY_V3_1_LLAMA_3_1;
    } else {
        data.format = COMMON_CHAT_FORMAT_CONTENT_ONLY;
    }

    data.prompt = apply(tmpl, inputs);
    // TODO: if (has_raw_python)
    return data;
}

static common_chat_params common_chat_params_init_hermes_2_pro(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    json extra_context = json {
        {"enable_thinking", inputs.enable_thinking},
    };
    extra_context.update(inputs.extra_context);

    data.prompt = apply(tmpl, inputs, /* messages_override =*/ std::nullopt, /* tools_override= */ std::nullopt, extra_context);
    data.format = COMMON_CHAT_FORMAT_HERMES_2_PRO;
    if (string_ends_with(data.prompt, "<think>\n")) {
        if (!extra_context["enable_thinking"]) {
            data.prompt += "</think>";
        } else {
            data.thinking_forced_open = true;
        }
    }

    if (!inputs.tools.is_null()) {
        // (content)?(<tool_call>{"name": "foo", "arguments": {"a": 1}}</tool_call>)*
        data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            std::vector<std::string> tool_rules;
            std::vector<std::string> tool_call_alts;
            std::vector<std::string> escaped_names;
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");
                builder.resolve_refs(parameters);
                tool_rules.push_back(builder.add_schema(name + "-call", {
                    {"type", "object"},
                    {"properties", json {
                        {"name", json {{"const", name}}},
                        {"arguments", parameters},
                    }},
                    {"required", json::array({"name", "arguments"})},
                }));
                tool_call_alts.push_back(builder.add_rule(
                    name + "-function-tag",
                    "\"<function\" ( \"=" + name + "\" | \" name=\\\"" + name + "\\\"\" ) \">\" space " +
                    builder.add_schema(name + "-args", parameters) + " "
                    "\"</function>\" space"));

                data.grammar_triggers.push_back({
                    COMMON_GRAMMAR_TRIGGER_TYPE_WORD,
                    "<function=" + name + ">",
                });
                auto escaped_name = regex_escape(name);
                data.grammar_triggers.push_back({
                    COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN,
                    "<function\\s+name\\s*=\\s*\"" + escaped_name + "\"",
                });
                escaped_names.push_back(escaped_name);
            });
            auto any_tool_call = builder.add_rule("any_tool_call", "( " + string_join(tool_rules, " | ") + " ) space");
            std::vector<std::string> alt_tags {
                any_tool_call,
                "\"<tool_call>\" space "     + any_tool_call + " \"</tool_call>\"",
                // The rest is just to accommodate common "good bad" outputs.
                "\"<function_call>\" space " + any_tool_call + " \"</function_call>\"",
                "\"<response>\"  space "     + any_tool_call + " \"</response>\"",
                "\"<tools>\"     space "     + any_tool_call + " \"</tools>\"",
                "\"<json>\"      space "     + any_tool_call + " \"</json>\"",
                "\"<xml>\"      space "     + any_tool_call + " \"</xml>\"",
                "\"<JSON>\"      space "     + any_tool_call + " \"</JSON>\"",
            };
            auto wrappable_tool_call = builder.add_rule("wrappable_tool_call", "( " + string_join(alt_tags, " | ") + " ) space");
            tool_call_alts.push_back(wrappable_tool_call);
            tool_call_alts.push_back(
                "( \"```\\n\" | \"```json\\n\" | \"```xml\\n\" ) space " + wrappable_tool_call + " space \"```\" space ");
            auto tool_call = builder.add_rule("tool_call", string_join(tool_call_alts, " | "));
            builder.add_rule("root",
                std::string(data.thinking_forced_open ? "( \"</think>\" space )? " : "") +
                (inputs.parallel_tool_calls ? "(" + tool_call + ")+" : tool_call));
            // Trigger on some common known "good bad" outputs (only from the start and with a json that's about a specific argument name to avoid false positives)
            data.grammar_triggers.push_back({
                COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL,
                // If thinking_forced_open, then we capture the </think> tag in the grammar,
                // (important for required tool choice) and in the trigger's first capture (decides what is sent to the grammar)
                std::string(data.thinking_forced_open ? "[\\s\\S]*?(</think>\\s*)" : "(?:<think>[\\s\\S]*?</think>\\s*)?") + (
                    "\\s*("
                    "(?:<tool_call>"
                    "|<function"
                    "|(?:```(?:json|xml)?\n\\s*)?(?:<function_call>|<tools>|<xml><json>|<response>)?"
                    "\\s*\\{\\s*\"name\"\\s*:\\s*\"(?:" + string_join(escaped_names, "|") + ")\""
                    ")"
                    ")[\\s\\S]*"
                ),
            });
            data.preserved_tokens = {
                "<think>",
                "</think>",
                "<tool_call>",
                "</tool_call>",
                "<function",
                "<tools>",
                "</tools>",
                "<response>",
                "</response>",
                "<function_call>",
                "</function_call>",
                "<json>",
                "</json>",
                "<JSON>",
                "</JSON>",
                "```",
                "```json",
                "```xml",
            };
        });
    }

    return data;
}

static common_chat_params common_chat_params_init_granite(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;

    // Pass thinking context for Granite template
    json additional_context = {
        {"thinking", inputs.enable_thinking},
    };

    data.prompt = apply(tmpl, inputs, /* messages_override= */ std::nullopt, /* tools_override= */ std::nullopt, additional_context);
    data.format = COMMON_CHAT_FORMAT_GRANITE;

    if (string_ends_with(data.prompt, "<think>\n") || string_ends_with(data.prompt, "<think>")) {
        if (!inputs.enable_thinking) {
            data.prompt += "</think>";
        } else {
            data.thinking_forced_open = true;
        }
    }

    if (!inputs.tools.is_null()) {
        // Granite uses <|tool_call|> followed by JSON list
        data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            std::vector<std::string> tool_rules;
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");
                builder.resolve_refs(parameters);
                tool_rules.push_back(builder.add_rule(name + "-call", builder.add_schema(name +
"-args", {
                    {"type", "object"},
                    {"properties", {
                        {"name", {{"const", name}}},
                        {"arguments", parameters},
                    }},
                    {"required", json::array({"name", "arguments"})},
                })));
            });

            auto tool_call = builder.add_rule("tool_call", string_join(tool_rules, " | "));
            auto tool_list = builder.add_rule("tool_list", "\"[\" space " + tool_call + " (\",\" space " + tool_call + ")* space \"]\"");

            if (data.thinking_forced_open) {
                builder.add_rule("root", "\"</think>\" space \"<response>\" space [^<]* \"</response>\" space \"<|tool_call|>\" space " + tool_list);
            } else {
                builder.add_rule("root", "\"<|tool_call|>\" space " + tool_list);
            }

            data.grammar_triggers.push_back({
                COMMON_GRAMMAR_TRIGGER_TYPE_WORD,
                "<|tool_call|>"
            });

            data.preserved_tokens = {
                "<think>",
                "</think>",
                "<response>",
                "</response>",
                "<|tool_call|>",
            };
        });
    } else {
        // Handle thinking tags for non-tool responses
        if (data.thinking_forced_open && inputs.enable_thinking) {
            data.grammar_lazy = false;
            data.grammar = build_grammar([&](const common_grammar_builder & builder) {
                builder.add_rule("root", "\"</think>\" space \"<response>\" space .* \"</response>\" space");
            });
            data.preserved_tokens = {
                "<think>",
                "</think>",
                "<response>",
                "</response>",
            };
        }
    }

    return data;
}

static common_chat_params common_chat_params_init_without_tools(const common_chat_template & tmpl, const struct templates_params & inputs) {
    common_chat_params data;
    data.prompt = apply(tmpl, inputs);
    data.format = COMMON_CHAT_FORMAT_CONTENT_ONLY;
    data.grammar_lazy = false;
    if (!inputs.json_schema.is_null()) {
        if (!inputs.grammar.empty()) {
            throw std::runtime_error("Either \"json_schema\" or \"grammar\" can be specified, but not both");
        }
        data.grammar = json_schema_to_grammar(inputs.json_schema);
    } else {
        data.grammar = inputs.grammar;
    }
    return data;
}

static common_chat_params common_chat_params_init_seed_oss(
    const common_chat_template         & tmpl,
    templates_params                   & params,
    const common_chat_templates_inputs & inputs)
{
    common_chat_params data;
    data.prompt = apply(tmpl, params);
    data.format = COMMON_CHAT_FORMAT_SEED_OSS;
    if (string_ends_with(data.prompt, "<seed:think>")) {
        if (!inputs.enable_thinking) {
            data.prompt += "</seed:think>";
        } else {
            data.thinking_forced_open = true;
        }
    }

    if (params.tools.is_array() && !params.tools.empty()) {
        data.grammar_lazy = inputs.tool_choice != COMMON_CHAT_TOOL_CHOICE_REQUIRED;
        data.grammar      = build_grammar([&](const common_grammar_builder & builder) {
            std::vector<std::string> tool_rules;
            foreach_function(params.tools, [&](const json & tool) {
                const auto & function   = tool.at("function");
                std::string  name       = function.at("name");
                auto         parameters = function.at("parameters");
                builder.resolve_refs(parameters);

                // Create rule for Seed-OSS function call format
                std::string param_rules;
                if (parameters.contains("properties")) {
                    for (const auto & [key, value] : parameters.at("properties").items()) {
                        param_rules += "\"<parameter=" + key + ">\"" + builder.add_schema(name + "-arg-" + key, value) +
                                       "\"</parameter>\"";
                    }
                }

                tool_rules.push_back(builder.add_rule(name + "-call",
                                                      "\"<seed:tool_call>\" space \"<function=" + name + ">\" space " +
                                                          param_rules +
                                                          " \"</function>\" space \"</seed:tool_call>\""));
            });

            data.grammar_triggers.push_back({ COMMON_GRAMMAR_TRIGGER_TYPE_WORD, "<seed:tool_call>" });

            data.preserved_tokens = {
                "<seed:think>", "</seed:think>", "<seed:tool_call>", "</seed:tool_call>",
                "<function=",   "</function>",   "<parameter=",      "</parameter>",
            };

            builder.add_rule("root", string_join(tool_rules, " | "));
        });
    }
    return data;
}

static common_chat_params common_chat_templates_apply_jinja(
    const struct common_chat_templates        * tmpls,
    const struct common_chat_templates_inputs & inputs)
{
    templates_params params;
    params.tools = common_chat_tools_to_json_oaicompat<json>(inputs.tools);
    const auto & tmpl = params.tools.is_array() && tmpls->template_tool_use
        ? *tmpls->template_tool_use
        : *tmpls->template_default;
    const auto & src = tmpl.source();
    const auto & caps = tmpl.original_caps();
    params.messages = common_chat_msgs_to_json_oaicompat<json>(inputs.messages, /* concat_text= */ !tmpl.original_caps().requires_typed_content);
    params.add_generation_prompt = inputs.add_generation_prompt;
    params.tool_choice = inputs.tool_choice;
    params.reasoning_format = inputs.reasoning_format;
    params.enable_thinking = inputs.enable_thinking;
    params.grammar = inputs.grammar;
    params.now = inputs.now;
    params.add_bos = tmpls->add_bos;
    params.add_eos = tmpls->add_eos;

    params.extra_context = json::object();
    for (auto el : inputs.chat_template_kwargs) {
        params.extra_context[el.first] = json::parse(el.second);
    }

    if (!inputs.json_schema.empty()) {
        params.json_schema = json::parse(inputs.json_schema);
    }

    if (inputs.parallel_tool_calls && !tmpl.original_caps().supports_parallel_tool_calls) {
        LOG_DBG("Disabling parallel_tool_calls because the template does not support it\n");
        params.parallel_tool_calls = false;
    } else {
        params.parallel_tool_calls = inputs.parallel_tool_calls;
    }

    if (params.tools.is_array()) {
        if (params.tool_choice != COMMON_CHAT_TOOL_CHOICE_NONE && !params.grammar.empty()) {
            throw std::runtime_error("Cannot specify grammar with tools");
        }
        if (caps.supports_tool_calls && !caps.supports_tools) {
            LOG_WRN("Template supports tool calls but does not natively describe tools. The fallback behaviour used may produce bad results, inspect prompt w/ --verbose & consider overriding the template.\n");
        }
    }

    // DeepSeek V3.1: detect based on specific patterns in the template
    if (src.find("message['prefix'] is defined and message['prefix'] and thinking") != std::string::npos &&
        params.json_schema.is_null()) {
        return common_chat_params_init_deepseek_v3_1(tmpl, params);
    }

    // DeepSeek R1: use handler in all cases except json schema (thinking / tools).
    if (src.find("<｜tool▁calls▁begin｜>") != std::string::npos && params.json_schema.is_null()) {
        return common_chat_params_init_deepseek_r1(tmpl, params);
    }

    // Command R7B: : use handler in all cases except json schema (thinking / tools).
    if (src.find("<|END_THINKING|><|START_ACTION|>") != std::string::npos && params.json_schema.is_null()) {
        return common_chat_params_init_command_r7b(tmpl, params);
    }

    // Granite (IBM) - detects thinking / tools support
    if (src.find("elif thinking") != std::string::npos && src.find("<|tool_call|>") != std::string::npos) {
        return common_chat_params_init_granite(tmpl, params);
    }

    // GLM 4.5: detect by <arg_key> and <arg_value> tags (check before Hermes since both use <tool_call>)
    if (src.find("[gMASK]<sop>") != std::string::npos &&
        src.find("<arg_key>") != std::string::npos &&
        src.find("<arg_value>") != std::string::npos &&
        params.json_schema.is_null()) {
        return common_chat_params_init_glm_4_5(tmpl, params);
    }

    // Qwen3-Coder XML format detection (must come before Hermes 2 Pro)
    // Detect via explicit XML markers unique to Qwen3-Coder to avoid false positives in other templates.
    // Require presence of <tool_call>, <function=...>, and <parameter=...> blocks.
    if (src.find("<tool_call>") != std::string::npos &&
        src.find("<function>") != std::string::npos &&
        src.find("<function=") != std::string::npos &&
        src.find("<parameters>") != std::string::npos &&
        src.find("<parameter=") != std::string::npos) {
        // Nemotron 3 Nano 30B A3B
        if (src.find("<think>") != std::string::npos) {
            return common_chat_params_init_nemotron_v3(tmpl, params);
        }
        return common_chat_params_init_qwen3_coder_xml(tmpl, params);
    }

    // Xiaomi MiMo format detection (must come before Hermes 2 Pro)
    if (src.find("<tools>") != std::string::npos &&
        src.find("# Tools") != std::string::npos &&
        src.find("</tools>") != std::string::npos &&
        src.find("<tool_calls>") != std::string::npos &&
        src.find("</tool_calls>") != std::string::npos &&
        src.find("<tool_response>") != std::string::npos) {
        return common_chat_params_init_xiaomi_mimo(tmpl, params);
    }

    // Hermes 2/3 Pro, Qwen 2.5 Instruct (w/ tools)
    if (src.find("<tool_call>") != std::string::npos && params.json_schema.is_null()) {
        return common_chat_params_init_hermes_2_pro(tmpl, params);
    }

    // GPT-OSS
    if (src.find("<|channel|>") != std::string::npos) {
        return common_chat_params_init_gpt_oss(tmpl, params);
    }

    // Seed-OSS
    if (src.find("<seed:think>") != std::string::npos) {
        return common_chat_params_init_seed_oss(tmpl, params, inputs);

    // Seed-OSS
    if (src.find("<seed:think>") != std::string::npos) {
        return common_chat_params_init_seed_oss(tmpl, params);
    }

    // Nemotron v2
@@ -2730,17 +886,6 @@ static common_chat_params common_chat_templates_apply_jinja(
        return common_chat_params_init_kimi_k2(tmpl, params);
    }

    // Apriel 1.5 format detection
    if (src.find("<thinking>") != std::string::npos &&
        src.find("</thinking>") != std::string::npos &&
        src.find("<available_tools>") != std::string::npos &&
        src.find("<|assistant|>") != std::string::npos &&
        src.find("<|tool_result|>") != std::string::npos &&
        src.find("<tool_calls>[") != std::string::npos &&
        src.find("]</tool_calls>") != std::string::npos) {
        return common_chat_params_init_apriel_1_5(tmpl, params);
    }

    // Use generic handler when mixing tools + JSON schema.
    // TODO: support that mix in handlers below.
    if ((params.tools.is_array() && params.json_schema.is_object())) {
@@ -2864,7 +1009,11 @@ common_chat_params common_chat_templates_apply(
    const struct common_chat_templates_inputs & inputs)
{
    GGML_ASSERT(tmpls != nullptr);
    return inputs.use_jinja
    common_chat_params params = inputs.use_jinja
        ? common_chat_templates_apply_jinja(tmpls, inputs)
        : common_chat_templates_apply_legacy(tmpls, inputs);
    if (!params.grammar_lazy && !params.grammar_triggers.empty()) {
        params.grammar_triggers.clear();
    }
    return params;
}
 9 changes: 4 additions & 5 deletions9  
common/chat.h
 160 changes: 151 additions & 9 deletions160  
common/peg-parser.cpp
 121 changes: 116 additions & 5 deletions121  
common/peg-parser.h
 38 changes: 26 additions & 12 deletions38  
common/preset.cpp
 102 changes: 45 additions & 57 deletions102  
docs/development/parsing.md
 74 changes: 74 additions & 0 deletions74  
docs/peg-testing-plan.md
 297 changes: 297 additions & 0 deletions297  
docs/pr-peg-migration.md
 191 changes: 191 additions & 0 deletions191  
docs/test-divergence-analysis.md
 22 changes: 22 additions & 0 deletions22  
docs/testing-chat-parsers.md
 8 changes: 8 additions & 0 deletions8  
models/templates/google-functiongemma.jinja
 1 change: 1 addition & 0 deletions1  
models/templates/unsloth-Nemotron-3-Nano.jinja
 9 changes: 5 additions & 4 deletions9  
scripts/fetch_server_test_models.py
 89 changes: 82 additions & 7 deletions89  
src/llama-grammar.cpp
 16 changes: 16 additions & 0 deletions16  
src/llama-grammar.h
 134 changes: 134 additions & 0 deletions134  
tests/peg-parser/test-basic.cpp
 31 changes: 31 additions & 0 deletions31  
tests/peg-parser/test-gbnf-generation.cpp
 68 changes: 34 additions & 34 deletions68  
tests/test-chat-peg-parser.cpp
 1,714 changes: 1,388 additions & 326 deletions1,714  
tests/test-chat.cpp
 21 changes: 21 additions & 0 deletions21  
tests/test-grammar-parser.cpp
 4 changes: 4 additions & 0 deletions4  
tools/server/tests/conftest.py
 147 changes: 97 additions & 50 deletions147  
tools/server/tests/unit/test_tool_call.py
Footer
© 2025 GitHub, Inc.
Footer navigation
Terms
Privacy
Security
Status
Community
Docs
Contact
Manage cookies
Do not share my personal information
