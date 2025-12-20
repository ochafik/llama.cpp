#pragma once

#include "chat.h"
#include "peg-parser.h"

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

// Alias for the tag enum
using Tag = common_chat_peg_tag;

// The builder now just inherits from the base - use p.tag(Tag::XXX, parser) directly
using common_chat_peg_builder = common_peg_parser_builder;

inline common_peg_arena build_chat_peg_parser(const std::function<common_peg_parser(common_chat_peg_builder & builder)> & fn) {
    common_chat_peg_builder builder;
    builder.set_root(fn(builder));
    return builder.build();
}

// Base mapper: handles reasoning and content tags
common_chat_peg_mapper common_chat_peg_base_mapper();

// Native mapper: handles tool calls with pre-parsed JSON args
common_chat_peg_mapper common_chat_peg_native_mapper();

// Constructed mapper: builds JSON args from individual parsed pieces
common_chat_peg_mapper common_chat_peg_constructed_mapper();

// FunctionGemma mapper: similar to constructed but uses <escape> delimited strings
common_chat_peg_mapper common_chat_peg_function_gemma_mapper();

// Convenience aliases for building parsers
inline common_peg_arena build_chat_peg_native_parser(const std::function<common_peg_parser(common_peg_parser_builder & builder)> & fn) {
    return build_chat_peg_parser(fn);
}

inline common_peg_arena build_chat_peg_constructed_parser(const std::function<common_peg_parser(common_peg_parser_builder & builder)> & fn) {
    return build_chat_peg_parser(fn);
}
