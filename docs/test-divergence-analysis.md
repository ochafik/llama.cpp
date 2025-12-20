# Test Divergence Analysis: PEG Migration Branch

## Overview

This document analyzes how tests have diverged from the `master` branch during the PEG parser migration. The core change is moving from "fallback parsers" (format enum only) to "always have a parser" (proper initialization with tools).

Branch point: `52392291b2f3a24b5d3fef4fc0b56f10db358dc1`

## 1. Tests Removed Entirely (Regression Risk)

### Seed OSS

**Multi-parameter tool call test:**
```cpp
// REMOVED - tested parsing tool with multiple string parameters
common_chat_msg msg_multi_param;
msg_multi_param.tool_calls.push_back({"process_data", "{\"input\": \"test\", \"format\": \"json\"}", ""});
assert_msg_equals(msg_multi_param,
    common_chat_parse(
        "<seed:tool_call>\n"
        "<function=process_data>\n"
        "<parameter=input>test</parameter>\n"
        "<parameter=format>json</parameter>\n"
        "</function>\n"
        "</seed:tool_call>", ...));
```

**Partial parsing test for incomplete tool calls:**
```cpp
// REMOVED - tested streaming behavior with incomplete JSON
assert_msg_equals(
    simple_assist_msg("", "", "calculate_sum", "{\"numbers\":"),
    common_chat_parse(
        "<seed:tool_call>\n"
        "<function=calculate_sum>\n"
        "<parameter=numbers>[1,\n",
        /* is_partial= */ true, ...));
```

### MiniMax M2

**Streaming test without reasoning format:**
```cpp
// REMOVED - tested parsing when reasoning_format = NONE
test_parser_with_streaming(message_assist_call_thoughts_unparsed,
    "<think>I'm\nthinking</think>\n\n<minimax:tool_call>...",
    [&](const std::string &msg) {
        return common_chat_parse(msg, true, {
            COMMON_CHAT_FORMAT_MINIMAX_M2,
            COMMON_REASONING_FORMAT_NONE  // <-- This case no longer tested
        });
    });
```

### Kimi K2

**Two streaming tests commented out:**
```cpp
// test_parser_with_streaming(
//     [&](const std::string &msg) { return common_chat_parse(msg, true, kimi_syntax_reasoning); });
```

**Status:** ✅ Investigated - these test **tool calls embedded inside `<think>` blocks**:
```cpp
"<think>I'm thinking<|tool_calls_section_begin|><|tool_call_begin|>functions.complex_function_in_think:0..."
```

This is a known limitation: the parser treats all content inside `<think>...</think>` as `reasoning_content`. Tool calls inside thinking blocks are not extracted. This is documented in the test file and is **not a regression** - it's an unimplemented edge case.

---

## 2. Tests Weakened (Reduced Coverage)

### Before vs After Comparison

| Aspect | Before (Fallback) | After (Always Parser) |
|--------|-------------------|----------------------|
| Function names | Arbitrary (`calculate_sum`, `process_data`, `fun`) | Must match predefined tools |
| Arguments | Arbitrary (`{"numbers": [1,2,3]}`) | Must match tool schema (`{"arg1":42}`) |
| Unknown tools | Could test parsing unknown functions | Cannot - parser rejects unknown tools |
| Schema validation | None | Full type checking |

### Impact

The old tests verified that the parser could extract tool calls from LLM output regardless of whether those tools were known. This was useful for:
- Robustness testing (malformed but parseable output)
- Edge cases (unexpected function names)
- Streaming partial results

The new tests only verify parsing when output exactly matches known tool schemas.

---

## 3. Behavioral Changes (Different Code Path)

### MiniMax M2: `thinking_forced_open`

**Before:**
```cpp
"<think>I'm\nthinking</think>Hello, world!"
```

**After:**
```cpp
"I'm\nthinking</think>Hello, world!"  // No <think> prefix
```

**Explanation:** When `thinking_forced_open=true`, the prompt already includes `<think>`, so LLM output starts with content directly. Tests now only cover this optimized path.

**Gap:** No longer testing parsing of complete `<think>...</think>` blocks when `thinking_forced_open=false`.

---

## 4. Format Changes (Whitespace Sensitivity)

### MiniMax M2 Tool Calls

**Before (compact):**
```xml
<minimax:tool_call><invoke name="special_function"><parameter name="arg1">1</parameter></invoke></minimax:tool_call>
```

**After (with newlines):**
```xml
<minimax:tool_call>
<invoke name="special_function">
<parameter name="arg1">1</parameter>
</invoke>
</minimax:tool_call>
```

**Impact:** Tests now only validate the parser's preferred/generated format, not flexible parsing of various whitespace patterns.

---

## 5. What Tests Gained

- ✅ Schema validation during parsing (type checking, required fields)
- ✅ Production-realistic flow (parser always initialized with tools)
- ✅ New format coverage: FunctionGemma, Nemotron-3 Nano
- ✅ Consistent test infrastructure across all formats

---

## 6. Action Items - Resolution

### High Priority - RESOLVED

1. ✅ **Re-add multi-parameter tests** for Seed OSS
   - Added `process_data_tool` with `input` and `format` string parameters
   - Added test for multi-parameter tool call parsing

2. ✅ **Kimi K2 streaming tests** - Documented
   - These test tool calls embedded inside `<think>` blocks
   - This is a known unimplemented edge case, not a regression
   - Parser treats all `<think>` content as reasoning

3. ✅ **`thinking_forced_open=false` for MiniMax M2** - By Design
   - MiniMax template always ends with `<think>\n`
   - When thinking is enabled, `thinking_forced_open` is always `true`
   - The `else` branch is dead code for this format - by design

### Medium Priority - RESOLVED

4. ✅ **Partial parsing tests** - Added
   - Added test for incomplete string parameter in Seed OSS
   - Verifies partial tool call captures partial argument value

5. ✅ **Whitespace flexibility** - Verified
   - Added compact format test for MiniMax M2
   - Parser correctly handles both compact and pretty-printed XML

---

## 7. Test Categories to Consider

### Category A: Schema-Validated Tests (Current)
- Parser initialized with tools
- Full type validation
- Production-realistic

### Category B: Lenient Parsing Tests (Missing)
- Parser initialized without tools or with permissive schema
- Tests format recognition without strict validation
- Useful for robustness testing

### Category C: Edge Case Tests (Partially Missing)
- Malformed but recoverable output
- Unknown function names
- Type mismatches (string where integer expected)
- Incomplete streaming data
