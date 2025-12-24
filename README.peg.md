# PEG Parser Limitations

This document tracks the current limitations of the PEG parser used for parsing LLM outputs in chat-parsers.

## JSON Schema Constraint Support

The PEG parser supports embedding JSON Schema constraints via `p.schema()`, which delegates to the GBNF grammar system in `json-schema-to-grammar.cpp`. However, some chat parsers bypass schema-aware parsing for certain types.

### Schema Constraints Supported by GBNF (json-schema-to-grammar.cpp)

| Constraint | Type | Status | Notes |
|------------|------|--------|-------|
| `minLength` | string | Supported | Character count constraints |
| `maxLength` | string | Supported | Character count constraints |
| `pattern` | string | Supported | Regex patterns |
| `minimum` | integer | Supported | Range constraints |
| `maximum` | integer | Supported | Range constraints |
| `exclusiveMinimum` | integer | Supported | Range constraints |
| `exclusiveMaximum` | integer | Supported | Range constraints |
| `minItems` | array | Supported | Array length constraints |
| `maxItems` | array | Supported | Array length constraints |
| `enum` | any | Supported | Enumerated values |
| `const` | any | Supported | Constant values |
| `minimum`/`maximum` | number (float) | Partial | TODO: needs implementation |

### Parser-Specific Limitations

#### Seed-OSS Parser (`seed-oss.cpp`)

The Seed-OSS parser currently uses `p.until("</parameter>")` for string parameters, which captures all content until the closing tag without enforcing schema constraints.

**Affected constraints for string parameters:**
- `minLength` - Not enforced
- `maxLength` - Not enforced
- `pattern` - Not enforced

**Workaround:** Use `p.schema()` for string parameters that need constraint enforcement:
```cpp
// Instead of:
arg_value = p.tag(Tag::TOOL_ARG_STRING_VALUE, p.until("</parameter>"));

// Use:
arg_value = p.tag(Tag::TOOL_ARG_JSON_VALUE, p.schema(p.json(), rule_name, param_schema));
```

**Note:** This changes the expected format from raw string to JSON string (quoted).

## Whitespace Handling

### GBNF Space Rule
The GBNF grammar uses a constrained space rule:
```
SPACE_RULE = ( " " | "\n"{1,2} [ \t]{0,20} )?
```

### PEG Newline Limits
PEG patterns should limit newline repetitions to prevent grammar from accepting unlimited whitespace:
```cpp
// Good: Limited newlines
auto gap = p.repeat(newline, 0, 2);

// Bad: Unlimited newlines (can cause "length" finish reason)
auto gap = p.repeat(newline, 0, -1);
```

## Known Issues

1. **String schema constraints in XML-style parsers**: Parsers that capture string content with `p.until()` (like Seed-OSS) don't enforce `minLength`, `maxLength`, or `pattern` constraints from the JSON schema.

2. **Required parameter enforcement**: Parameters marked as `required` in the schema should use `p.repeat(arg_rule, 1, 1)` to enforce their presence in the grammar.

## Future Improvements

- [ ] Add `p.until_max(delimiter, max_length)` for length-constrained string capture
  - Parser: Match up to max_length characters until delimiter
  - GBNF: Generate `[^<]{0,max_length}` or similar pattern
  - Alternative: Use `p.schema()` for strings with constraints (changes format to JSON-quoted)
- [ ] Add schema-aware string parsing option for XML-style formats
- [ ] Support `minimum`/`maximum` for floating-point numbers
- [ ] Add `multipleOf` constraint support for numbers

## Implementation Notes

### Adding `max_length` to Until Parser

The `common_peg_until_parser` struct could be extended:
```cpp
struct common_peg_until_parser {
    std::vector<std::string> delimiters;
    int max_length = -1;  // -1 for unbounded
};
```

In GBNF generation (peg-parser.cpp):
```cpp
if (p.max_length > 0) {
    // Simplified: use first char of delimiter as exclusion
    char first = p.delimiters[0][0];
    return "[^" + std::string(1, first) + "]{0," + std::to_string(p.max_length) + "}";
}
return gbnf_excluding_pattern(p.delimiters);
```
