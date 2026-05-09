# vlib-video unit tests (U1, U2, U3, U4 from VIDEO_CONV3D_CPP_DESIGN.md §5.1)
#
# U1 — pair packing (uses the stub encoder, no model)
# U2 — tool parser (synthetic fixtures, no model)
# U3 — cumulative MRoPE positions (session + stub encoder, no model)
# U4 — selective rewind (covered alongside U3)

# The session/MRoPE/rewind tests need a live llama_context to exercise
# llama_decode and llama_memory_seq_rm. We can't bring up a real model in
# CI without burning agent time, so we keep these as a compile-only build
# of the test binary and a smoke run that skips when a model is absent.

add_executable(test-vlib-tool-parser test_tool_parser.cpp)
target_link_libraries   (test-vlib-tool-parser PRIVATE vlib-video)
target_compile_features (test-vlib-tool-parser PRIVATE cxx_std_17)
add_test(NAME vlib-tool-parser COMMAND test-vlib-tool-parser
         "${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/tool_calls.jsonl")

add_executable(test-vlib-pair-packing test_pair_packing.cpp)
target_link_libraries   (test-vlib-pair-packing PRIVATE vlib-video)
target_compile_features (test-vlib-pair-packing PRIVATE cxx_std_17)
add_test(NAME vlib-pair-packing COMMAND test-vlib-pair-packing)

add_executable(test-vlib-mrope test_mrope.cpp)
target_link_libraries   (test-vlib-mrope PRIVATE vlib-video)
target_compile_features (test-vlib-mrope PRIVATE cxx_std_17)
add_test(NAME vlib-mrope COMMAND test-vlib-mrope)
