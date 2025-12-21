# PEG Parser Testing Plan

## Background and current behavior
- `common_chat_parse()` (common/chat-parser.cpp:1505-1528) automatically falls back to legacy, non-PEG parsing unless `common_chat_syntax.parser` is populated. Any parser test that forgets to load the serialized PEG arena therefore bypasses the very code we want to exercise.
- Every template-specific `common_chat_params_init_*` helper (see common/chat-parsers/*.cpp) emits a serialized PEG parser through `common_chat_params.parser`. The parser adapts itself to the selected capabilities (tools enabled/disabled, reasoning enabled/disabled, forced-thinking modes, schema-based responses, etc.).
- tests/test-chat.cpp already has two relevant harnesses:
  - `test_templates()` and `make_peg_parser`/`test_peg_parser()` render a template, load the PEG arena, and run full/streaming parses against golden chat messages.
  - The new `test_streaming_with_needles()` infrastructure pushes character-by-character inputs through `common_chat_parse()` while tracking the “needle” markers to prove true incremental streaming.
- At the moment, `test_systematic_needle_streaming()` (tests/test-chat.cpp:4190+) always builds the parser with one specific input recipe: tools enabled (Python tool), tool choice `AUTO`, parallel tool calls disabled, and thinking enabled whenever the template advertises support. It exercises content-only streaming, a reasoning-only mode (if thinking is supported), and the “thinking disabled” fallback — but only inside that single combination of tool/thinking flags.

## Gaps we need to close
1. **PEG arena coverage** – the needle tests call `common_chat_parse()` and rely on the parser that was loaded once at the beginning of the template loop. When we change knobs (e.g., disable thinking), we often keep reusing the old `common_chat_syntax` object. If a format requires a different parser for “no tools” or “no thinking,” the tests silently fall back to the legacy parser. We need the tests to fail loudly whenever we forget to load the PEG arena for the specific scenario under test.
2. **Tools vs. no-tools** – a large subset of formats produces different PEG grammars depending on whether `inputs.tools` is empty and whether `tool_choice` allows tool calls. Today’s needle suite never renders the “no tools present” parser: it always injects the Python tool. That means we never check that the content-only parser increments correctly when templates omit tool support, nor that tool-specific constructs disappear cleanly when tools are disabled.
3. **Thinking vs. no-thinking** – while we have a “thinking disabled” pass, we only validate the plain-content streaming path. We never check that: (a) reasoning-only segments stream incrementally when thinking is enabled but `tool_choice=NONE`, (b) reasoning is suppressed entirely when thinking is disabled, or (c) formats that force-open reasoning (e.g., Granite when the template ends with `<think>`) behave correctly when we explicitly disable thinking. We also skip verifying the content path for templates whose `params.thinking_forced_open` is `true` even though they still have a “no thinking” parser variant.
4. **Tool-call streaming** – the TODO at tests/test-chat.cpp:4433-4435 leaves every tool-call needle test disabled, so we never validate that PEG tool-call parsers stream arguments incrementally, keep tool IDs stable, or obey `parallel_tool_calls` limits.
5. **Matrix coverage** – combinations like “tools provided but `tool_choice=NONE`,” “schema-bound tool calls,” or “parallel tool calls allowed” are untested even though the PEG builders have dedicated logic for each branch.

## Proposed coverage matrix
For every template in `test_systematic_needle_streaming()`, run the needles across the following scenarios (skipping the cases that the template cannot support based on `ThinkingSupport`/`ToolSupport`):

| Scenario | Tools provided? | Tool choice | Thinking enabled? | Reasoning format | Expected stream | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| C1 | No | NONE | false | NONE | Content only | Baseline parser with pure text; should always load a PEG arena and assert it is non-empty. |
| C2 | No | NONE | true (if supported) | DEEPSEEK | Reasoning + content | Covers templates whose PEG parser switches when thinking is enabled but no tools exist. |
| C3 | Yes | NONE | matches template default | NONE | Content only | Proves that providing tools but forbidding tool calls still yields pure text outputs. |
| T1 | Yes | AUTO | default | NONE | Content + tool call | Exercise single-tool streaming (add the missing format-specific tool-call needle generator). |
| T2 | Yes | REQUIRED | default | NONE | Pure tool call output (no assistant content) | Verifies that the parser handles tool-only completions and that the “arguments never regress” invariant holds when the call is the first token emitted. |
| P1 | Yes | AUTO | default | NONE | Parallel tool calls | Only for templates that advertise parallel support; ensures PEG parser repeats correctly. |
| R1 | (any) | (any) | true | DEEPSEEK | Reasoning forced-open | Use templates whose `params.thinking_forced_open` is true to verify streaming with no content prefix. |
| R2 | (any) | (any) | false | NONE | No reasoning allowed | Confirms that the reasoning needles never appear when thinking is disabled.

We can collapse some rows when a template lacks the capability (e.g., no need to run T1/T2/P1 when `ToolSupport::No`). The important part is that the test harness actively iterates over “tool availability × tool choice × thinking flag” and loads the correct PEG arena for each permutation.

## Harness changes
1. **Explicit PEG loader per scenario** – extend `test_streaming_with_needles()` so it receives a `std::function<common_chat_msg(const std::string &)>` plus metadata about the scenario. The helper that builds this function should:
   - Create a fresh `common_chat_templates_inputs` structure for each permutation (tools/thinking/parallel/tool_choice).
   - Call `common_chat_templates_apply()` to obtain the serialized parser.
   - Load the parser into a dedicated `common_chat_syntax` instance and assert that the arena is not empty.
   - Encapsulate `common_chat_peg_parse()` (instead of `common_chat_parse()`) to guarantee we never fall back onto the legacy parser.
2. **Tool-call context generation** – add a `build_tool_stream_sample()` helper alongside `make_needle_context()` that produces the correct raw output for each format. We already have the `test_templates()` machinery that renders a template for a provided `common_chat_msg`; we can reuse the same approach to produce an assistant reply that:
   - Emits content and/or reasoning needles.
   - Emits a tool call whose JSON arguments include the argument needles.
   - Handles format-specific wrappers (e.g., XML envelopes, `<|START_ACTION|>` blocks, Apertus short-form calls). Once the raw string is rendered, feed it straight into `test_streaming_with_needles()`.
3. **Scenario driver** – replace the current loop body in `test_systematic_needle_streaming()` with something like:
   ```cpp
   for (const auto & scenario : enumerate_scenarios(tmpl_info)) {
       auto tmpls = read_templates(...);
       auto parser_fn = make_peg_parser_for_scenario(tmpls.get(), scenario);
       auto ctx = build_needle_context(scenario);
       auto result = test_streaming_with_needles(ctx.rendered_text, parser_fn, scenario.expected_tool_name);
       verify_needle_results(result, ctx);
   }
   ```
   `enumerate_scenarios()` will yield the matrix rows (C1, C2, …) depending on the template capabilities.
4. **Strict assertions** – add guards that fail the test when:
   - `scenario.requires_tools` but the template does not support them (this catches stale metadata).
   - `common_chat_templates_apply()` returns an empty `parser` string (indicating the template path does not yet have a PEG parser).
   - `syntax.parser.empty()` before streaming (preventing legacy fallback).
5. **Parallel tool-call support** – when `scenario.parallel_tool_calls` is true, stream a response with two tool calls (with individual needles in each call) and ensure `test_streaming_with_needles()` iterates over the vector rather than assuming a single call.

## Parser guardrails
- Once a grammar trigger (e.g. `<seed:tool_call>`) fires, the PEG definition must not offer “escape hatches” such as an alternate `.*` rule that treats the remainder as plain content. Grammar-constrained sampling keeps every alternative parse stack alive; leaving a fallback path effectively disables the constraint. Handle model misbehavior (wrong function name, missing tool call, bad argument types) outside the PEG parser by rejecting the output or surfacing a clear error.

## Incremental rollout plan
1. **Refactor harness** – introduce the scenario structs, parser builder, and strict PEG-loading asserts without expanding the matrix yet. This guarantees every existing needle test is already hitting the PEG implementation.
2. **Implement tool-call needle generator** – extend `make_needle_context()` to emit format-aware tool call strings and re-enable the currently skipped “Test 4” block.
3. **Add the matrix rows gradually** – start with C1/C3/T1/R1 for a single representative template (e.g., Command R7B) to keep runtime manageable, then expand to all templates.
4. **Optimize runtime** – cache rendered template outputs per `(template, scenario)` pair so that we do not re-render the entire prompt for each streaming character. The existing `test_templates()` logic already shows how to diff prompts to extract the assistant delta; we can reuse that to build the streaming inputs once and feed them to every relevant scenario.
5. **Extend to schema-based formats** – once the base matrix is stable, add another scenario for templates that return JSON schema-constrained content or function arguments to ensure the PEG parser’s schema integration is exercised.

## Deliverables
- Updated `tests/test-chat.cpp` with the scenario matrix, PEG-only parser harness, and tool-call streaming tests.
- Enhanced `needle_test_context` (and helpers) to produce format-aware tool-call payloads, including multiple tool calls when required.
- Optional but recommended: documentation in `docs/testing-chat-parsers.md` summarizing the new scenario coverage so future contributors know how to add formats and scenarios without regressing PEG coverage.
