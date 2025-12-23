# PR: Modular PEG-based chat parsers

## Summary

This PR refactors chat template parsing from a monolithic implementation to a modular, PEG-based architecture:

- **Modular parsers**: Each of the 26 supported formats now has its own file in `common/chat-parsers/`
- **Unified parsing & grammar**: PEG definitions generate both the streaming parser AND the GBNF grammar
- **Token-aware matching**: New `token()` primitive for matching special tokens atomically
- **Comprehensive tests**: Needle streaming tests validate incremental parsing AND final output for 20 templates

**Net result**: `chat.cpp` reduced from 2730 to ~1000 lines. Code is more maintainable, testable, and extensible.

---

## Motivation

### Problems with the old architecture

1. **Monolithic `chat.cpp`**: All 26 format implementations in one 2700-line file
2. **Duplicate definitions**: Parser logic and grammar generation were separate, often duplicated
3. **Hard to test**: No systematic streaming tests; partial parsing bugs were common
4. **XML utility complexity**: The `xml_tool_call_format` abstraction was hard to understand and customize

### Benefits of the new architecture

1. **One file per format**: Each parser is 60-200 lines, self-contained and easy to understand
2. **Single source of truth**: PEG definition generates both parser and grammar
3. **Streaming-first**: All parsers handle partial input correctly by design
4. **Easy to extend**: Adding a new format is copy-paste + modify

---

## Changes

### New files

| File | Description |
|------|-------------|
| `common/chat-parsers/*.cpp` | 26 per-format parser implementations |
| `common/chat-parsers-internal.h` | Shared types and helpers for parsers |

### Modified files

| File | Changes |
|------|---------|
| `common/chat.cpp` | Reduced to format detection + routing (~1000 lines) |
| `common/peg-parser.h` | Added `token()`, enum-based tags, helper methods |
| `common/chat-peg-parser.cpp` | Consolidated AST→message mappers |
| `tests/test-chat.cpp` | Added needle streaming tests for 20 templates |

### Deleted files

| File | Reason |
|------|--------|
| `common/chat-parser-xml-toolcall.cpp/h` | Replaced by inline PEG parsing |

---

## Architecture

### Before

```
chat.cpp (2730 lines)
├── Format detection
├── 26 inline parser implementations
├── 26 inline grammar builders
└── Fallback parsing logic

chat-parser-xml-toolcall.cpp (879 lines)
└── Shared XML grammar generation
```

### After

```
chat.cpp (~1000 lines)
├── Format detection
└── Routing to modular parsers

chat-parsers/
├── mistral-nemo.cpp      (80 lines)
├── hermes-2-pro.cpp      (202 lines)
├── minimax-m2.cpp        (128 lines)
└── ... (26 files, ~3350 lines total)

chat-parsers-internal.h
└── Shared types & helpers
```

---

## PEG Parser Enhancements

### Token-aware matching

Special tokens like `<tool_call>` should be matched atomically. The new `token()` primitive:

```cpp
// Old: text-based matching (could match partial token)
p.literal("<tool_call>")

// New: token-aware matching (matches whole token or falls back to text)
p.token("<tool_call>")
```

### Enum-based tags

Tags for AST nodes now use enums for fast switch-based dispatch:

```cpp
// Old: string comparison
if (node.tag == "TOOL_NAME") { ... }

// New: enum switch
switch (static_cast<Tag>(node.tag_id)) {
    case Tag::TOOL_NAME: ...
}
```

### Helper methods

Common patterns are now one-liners:

```cpp
// Combines atomic() + tag() + token()
p.token_tag(Tag::TOOL_OPEN, "<tool_call>")

// Combines atomic() + tag() + literal()
p.literal_tag(Tag::TOOL_NAME, "function_name")
```

---

## Example: MiniMax-M2 Parser

```cpp
// Format: <minimax:tool_call><invoke name="func"><parameter name="key">value</parameter></invoke></minimax:tool_call>

auto parser = build_chat_peg_parser([&](auto & p) {
    using Tag = common_chat_peg_tag;

    // Tool call structure
    auto tool_open = "<invoke name=\"" + p.literal_tag(Tag::TOOL_NAME, name) + "\">";
    auto arg = "<parameter name=\"" + p.literal_tag(Tag::TOOL_ARG_NAME, param) + "\">"
             + p.tag(Tag::TOOL_ARG_STRING_VALUE, p.until("</parameter>"))
             + "</parameter>";
    auto tool_close = "</invoke>";

    // Full parser with optional reasoning
    return reasoning
        << p.tag(Tag::CONTENT, p.until("<minimax:tool_call>"))
        << p.token_tag(Tag::TOOL_OPEN, "<minimax:tool_call>")
        << tool_calls
        << p.token_tag(Tag::TOOL_CLOSE, "</minimax:tool_call>");
});

// Same definition generates GBNF grammar automatically!
data.grammar = build_grammar([&](auto & builder) {
    parser.build_grammar(builder, /*lazy=*/true);
});
```

---

## Testing

### Needle streaming tests

Each template is tested with "needle" markers that must appear in order during streaming:

```cpp
// Input with needles in content, reasoning, and tool arguments
"<think>Thinking $N1R$ deeply $N2R$</think>Content $N1C$ here $N2C$"

// Test verifies streaming behavior:
// 1. Needles appear in order (N1 before N2) for each field
// 2. Content, reasoning, and tool args all stream incrementally
// 3. Tool arguments never regress (only grow)

// Test also verifies final output correctness:
// 4. Final content matches expected value
// 5. Final reasoning_content matches expected value
// 6. Final tool calls have correct name and arguments
```

### Scenario-based testing

Tests are generated from scenarios that cover all combinations:

| Scenario | Content | Reasoning | Tools | Description |
|----------|---------|-----------|-------|-------------|
| `content-no-tools` | ✓ | ✗ | ✗ | Basic content streaming |
| `content-with-reasoning` | ✓ | ✓ | ✗ | Thinking + content |
| `reasoning-only` | ✗ | ✓ | ✗ | Just reasoning block |
| `thinking-disabled` | ✓ | ✗ | ✗ | Explicit disable |
| `tool-auto-single` | ✓ | ✗ | 1 | Single tool call |
| `tool-required-only` | ✗ | ✗ | 1 | Tool only, no content |
| `parallel-tool-calls` | ✓ | ✗ | 2 | Multiple tool calls |
| `tool-with-reasoning` | ✓ | ✓ | 1 | All three combined |

### Coverage

| Category | Templates Tested |
|----------|-----------------|
| With thinking | Command-R7B, DeepSeek-R1, Granite, Hermes-2-Pro, MiniMax-M2, ... |
| Without thinking | Mistral-Nemo, Llama-3.x, Firefunction-V2, ... |
| Total | 20 templates |

### Complete parsing tests

In addition to streaming tests, `test_template_output_peg_parsers()` verifies complete parsing for each format:
- Plain content
- Content with reasoning
- Single and parallel tool calls
- Tool calls with complex arguments (nested objects, code with escapes)
- JSON response format with schemas

---

## Supported Formats

All 26 formats are now modular:

| Format | File | Notes |
|--------|------|-------|
| Mistral Nemo | `mistral-nemo.cpp` | JSON array tool calls |
| Magistral | `magistral.cpp` | With `[THINK]` reasoning |
| Hermes 2 Pro | `hermes-2-pro.cpp` | Multiple tool call syntaxes |
| Llama 3.x | `llama-3-x.cpp` | With builtin tools support |
| DeepSeek R1 | `deepseek-r1.cpp` | `<think>` reasoning |
| DeepSeek V3.1 | `deepseek-v3-1.cpp` | `<think>` + JSON tools |
| Command R7B | `command-r7b.cpp` | `<\|START_THINKING\|>` reasoning |
| MiniMax-M2 | `minimax-m2.cpp` | XML invoke/parameter format |
| Qwen3-Coder | `qwen3-coder-xml.cpp` | XML function/parameter format |
| GLM 4.5 | `glm-4-5.cpp` | XML arg_key/arg_value format |
| Kimi K2 | `kimi-k2.cpp` | Special token delimiters |
| Granite | `granite.cpp` | `<think>` + tool calls |
| FunctionGemma | `function-gemma.cpp` | `<escape>` value delimiters |
| Nemotron V2/V3 | `nemotron-*.cpp` | XML function format |
| Seed OSS | `seed-oss.cpp` | `<seed:tool_call>` format |
| Functionary | `functionary-*.cpp` | Various JSON formats |
| Firefunction V2 | `firefunction-v2.cpp` | `functools[...]` format |
| GPT-OSS | `gpt-oss.cpp` | Channel-based format |
| LFM2 | `lfm2.cpp` | `<\|tool_call_start\|>` format |
| Apertus | `apertus.cpp` | `<\|tools_prefix\|>` format |
| Apriel 1.5 | `apriel-1-5.cpp` | `<tool_calls>` JSON wrapper |
| Xiaomi MiMo | `xiaomi-mimo.cpp` | Standard tool format |
| Generic | `generic.cpp` | Fallback JSON format |

---

## Migration Guide

### For maintainers

Adding a new format:

1. Create `common/chat-parsers/myformat.cpp`
2. Implement `common_chat_params_init_myformat()`
3. Add detection logic in `chat.cpp`
4. Add forward declaration in `chat-parsers-internal.h`
5. Add test case in `test-chat.cpp`

### For downstream users

No API changes. The `common_chat_parse()` function works exactly as before.

---

## Performance

No measurable performance regression. The PEG parser was already used; this PR just:
- Moves code to separate files (no runtime change)
- Uses enum tags instead of strings (slightly faster)
- Adds token-aware matching (same speed, better correctness)

---

## Future Work

- [ ] Add tool call streaming tests (format-specific tool call syntax generation)
- [ ] Consider XML helper methods for common patterns (optional)
- [ ] Profile grammar generation for very large tool schemas

---

## Checklist

- [x] All 26 formats migrated to modular parsers
- [x] `chat-parser-xml-toolcall.*` deleted (replaced by inline PEG)
- [x] Needle streaming tests for 20 templates
- [x] Streaming tests verify both incremental behavior AND final output correctness
- [x] Complete parsing tests for each format
- [x] All existing tests pass
- [x] No API changes for downstream users
