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
- `<<<N1C>>>`, `<<<N2C>>>` - Content needles
- `<<<N1R>>>`, `<<<N2R>>>` - Reasoning needles
- `<<<N1AK>>>_N`, `<<<N2AK>>>_N` - Arg key needles
- `<<<N1AV>>>_N`, `<<<N2AV>>>_N` - Arg value needles

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
