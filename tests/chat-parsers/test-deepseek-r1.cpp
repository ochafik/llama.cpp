#include "../test-chat.h"

void test_deepseek_r1_parser(chat_parser_impl impl)
{
    printf("[%s]\n", __func__);

    common_chat_templates_inputs inputs_no_tools;
    inputs_no_tools.messages                = {message_user};

    common_chat_templates_inputs inputs_tools;
    inputs_tools.messages                   = {message_user};
    inputs_tools.tools                      = {special_function_tool};

    common_chat_templates_inputs inputs_tools_builtin;
    inputs_tools_builtin.messages           = {message_user};
    inputs_tools_builtin.tools              = {python_tool};

    {
        // Templates with thinking support
        template_capabilities template_caps;
        template_caps.name = "DeepSeek R1";
        template_caps.jinja_path = "models/templates/deepseek-ai-DeepSeek-R1-Distill-Llama-8B.jinja";
        template_caps.legacy_format = COMMON_CHAT_FORMAT_DEEPSEEK_R1;
        template_caps.experimental_format = COMMON_CHAT_FORMAT_PEG_NATIVE;
        template_caps.supports_thinking = ThinkingSupport::Yes;
        template_caps.think_open_tag = "<think>";
        template_caps.think_close_tag = "</think>";
        template_caps.reasoning_requires_tools = ReasoningRequiresTools::No;
        template_caps.tools_emit_content_with_calls = ToolsEmitContentWithCalls::No;
        template_caps.inject_reasoning_after_format = InjectReasoningAfterFormat::Yes;
        
        auto tmpls = read_templates(template_caps.jinja_path);
        test_systematic_needle_streaming(impl, template_caps, tmpls);
    }
    {
        // Replacement DeepSeek R1 template. Makes the Distill Qwen 7B/32B models happy to call tools and all.
        template_capabilities template_caps;
        template_caps.name = "DeepSeek R1 (fixed)";
        template_caps.jinja_path = "models/templates/llama-cpp-deepseek-r1.jinja";
        template_caps.legacy_format = COMMON_CHAT_FORMAT_DEEPSEEK_R1;
        template_caps.experimental_format = COMMON_CHAT_FORMAT_PEG_NATIVE;
        template_caps.supports_thinking = ThinkingSupport::Yes;
        template_caps.think_open_tag = "<think>";
        template_caps.think_close_tag = "</think>";
        template_caps.reasoning_requires_tools = ReasoningRequiresTools::No;
        template_caps.tools_emit_content_with_calls = ToolsEmitContentWithCalls::No;
        template_caps.inject_reasoning_after_format = InjectReasoningAfterFormat::Yes;
        template_caps.supports_disable_thinking = SupportsDisableThinking::No;
        template_caps.supports_reasoning_only = SupportsReasoningOnly::No;

        auto tmpls = read_templates(template_caps.jinja_path);
        test_systematic_needle_streaming(impl, template_caps, tmpls);

        std::vector<std::string>   end_tokens{ "<｜end▁of▁sentence｜>" };

        assert_equals(COMMON_CHAT_FORMAT_DEEPSEEK_R1,                   common_chat_templates_apply(tmpls.get(), inputs_no_tools).format);
        assert_equals(COMMON_CHAT_FORMAT_DEEPSEEK_R1,                   common_chat_templates_apply(tmpls.get(), inputs_tools).format);

        test_templates(impl, tmpls.get(), end_tokens, message_assist, tools, "Hello, world!\nWhat's up?", /* expect_grammar_triggered= */ false);
        test_templates(impl, tmpls.get(), end_tokens, message_assist_thoughts, tools, "Hello, world!\nWhat's up?", /* expect_grammar_triggered= */ false);
        assert_msg_equals(message_assist_thoughts_unparsed_deepseek,
            common_chat_parse(
                "<think>I'm\nthinking</think>Hello, world!\nWhat's up?",
                /* is_partial= */ false,
                {COMMON_CHAT_FORMAT_DEEPSEEK_R1}));
        assert_msg_equals(message_assist_thoughts,
            common_chat_parse(
                "<think>I'm\nthinking</think>Hello, world!\nWhat's up?",
                /* is_partial= */ false,
                {
                    /* .format = */ COMMON_CHAT_FORMAT_DEEPSEEK_R1,
                    /* .reasoning_format = */ COMMON_REASONING_FORMAT_DEEPSEEK,
                }));
        assert_msg_equals(message_assist_thoughts,
            common_chat_parse(
                "I'm\nthinking</think>Hello, world!\nWhat's up?",
                /* is_partial= */ false,
                {
                    /* .format = */ COMMON_CHAT_FORMAT_DEEPSEEK_R1,
                    /* .reasoning_format = */ COMMON_REASONING_FORMAT_DEEPSEEK,
                    /* .reasoning_in_content = */ false,
                    /* .thinking_forced_open = */ true,
                }));

        assert_msg_equals(message_assist_call_thoughts_unparsed,
            common_chat_parse(
                "<think>I'm\nthinking</think>\n\n"
                "<｜tool▁calls▁begin｜><｜tool▁call▁begin｜>function<｜tool▁sep｜>special_function\n"
                "```json\n"
                "{\"arg1\": 1}\n"
                "```<｜tool▁call▁end｜><｜tool▁calls▁end｜>",
                /* is_partial= */ false,
                {COMMON_CHAT_FORMAT_DEEPSEEK_R1}));
        assert_msg_equals(message_assist_call,
            common_chat_parse(
                "<｜tool▁calls｜>function<｜tool▁sep｜>special_function\n"
                "```json\n"
                "{\"arg1\": 1}\n"
                "```<｜tool▁call▁end｜><｜tool▁calls▁end｜>",
                /* is_partial= */ false,
                {COMMON_CHAT_FORMAT_DEEPSEEK_R1}));

        assert_msg_equals(message_assist_call_thoughts,
            common_chat_parse(
                "<think>I'm\nthinking</think>\n\n"
                "<｜tool▁calls▁begin｜><｜tool▁call▁begin｜>function<｜tool▁sep｜>special_function\n"
                "```json\n"
                "{\"arg1\": 1}\n"
                "```<｜tool▁call▁end｜><｜tool▁calls▁end｜>",
                /* is_partial= */ false,
                {
                    /* .format = */ COMMON_CHAT_FORMAT_DEEPSEEK_R1,
                    /* .reasoning_format = */ COMMON_REASONING_FORMAT_DEEPSEEK,
                }));
        test_templates(impl, tmpls.get(), end_tokens, message_assist_call, tools,
                "<｜tool▁calls▁begin｜><｜tool▁call▁begin｜>function<｜tool▁sep｜>special_function\n"
                "```json\n"
                "{\"arg1\": 1}\n"
                "```<｜tool▁call▁end｜><｜tool▁calls▁end｜>");
    }
}
