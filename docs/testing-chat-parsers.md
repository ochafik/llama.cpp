Given the extensive refactoring that's been happening in common/chat-syntax (peg parsers for each former adhoc parser in common/chat.cpp), we want to create extensive parsing.

There was some of this in tools/server/tests/unit/test_tool_call.py (including actual streaming test), but tied to real models so slow to run, and w/ increasingly poor testing coverage.

Also, there's some parsing in Minja (https://github.com/ochafik/minja) but w/ limited input types (see https://github.com/ochafik/minja/tree/main/tests/contexts), and not covering our streaming of course.

So we want to:
- have a rich set of test cases
- ensure they get fully parsed properly
- ensure they get streamed properly
  - values not coming back in one go
  - partial json of tool call args is strictly incremental (no parsing "regressions")
  - tool call names never split up

For that we can use the same idea as minja's contexts (injected in templates to generate output, then output is parsed in full mode, and in streaming drip-drip char by char mode), with the following twist:
- Needling tests:
  - Each field will have 2 "needles": streaming tests will make sure we detect the first needle before detecting the second, for each input type (e.g. for thinking, content, tool call argument)
- Forced thinking tested systematically
  - for each template that supports it!
- Adhoc tests
  - JSON Front matter w/ template name, expected output
  - Body w/ edge cases for gnarly escaping cases, etc.
