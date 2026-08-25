#ifdef NDEBUG
#    undef NDEBUG
#endif

// Regression tests for lazy-grammar trigger arming.
//
// A lazy grammar arms when a trigger pattern appears in the generated text. The
// text following the trigger was sampled unconstrained, so the "trigger" may be
// a quotation of the marker rather than an actual payload start — e.g. a model
// reciting tool-use instructions that contain the marker verbatim (observed
// with the DeepSeek V4-Flash tools header, which embeds a full example block).
// Arming must be speculative: commit only if the buffered continuation parses,
// otherwise roll back and keep watching for later occurrences.

#include "sampling.h"

#include <cassert>
#include <string>
#include <vector>

static const llama_vocab * vocab;

static llama_sampler * make_lazy_sampler(const char * gbnf, const char * trigger_pattern) {
    const char * patterns[] = { trigger_pattern };
    return llama_sampler_init_grammar_lazy_patterns(vocab, gbnf, "root", patterns, 1, nullptr, 0);
}

// Feed a string token-by-token. Returns false if any of its tokens was masked
// out by the sampler at the point it was accepted (i.e. the grammar was armed
// and rejected it).
static bool accept_string(llama_sampler * smpl, const std::string & text) {
    auto tokens = common_tokenize(vocab, text, false, false);

    const auto n_vocab = llama_vocab_n_tokens(vocab);
    std::vector<llama_token_data> cur;
    cur.reserve(n_vocab);
    for (llama_token token_id = 0; token_id < (llama_token) n_vocab; token_id++) {
        cur.emplace_back(llama_token_data{ token_id, 0.0f, 0.0f });
    }

    for (const auto token : tokens) {
        for (llama_token token_id = 0; token_id < (llama_token) n_vocab; token_id++) {
            cur[token_id].logit = 0.0f;
        }
        auto tok_arr = llama_token_data_array{ cur.data(), cur.size(), -1, false };
        llama_sampler_apply(smpl, &tok_arr);
        if (cur[token].logit < 0.0f) {
            return false;
        }
        llama_sampler_accept(smpl, token);
    }
    return true;
}

// True iff the sampler currently allows end-of-generation.
static bool eog_allowed(llama_sampler * smpl) {
    const auto n_vocab = llama_vocab_n_tokens(vocab);
    std::vector<llama_token_data> cur;
    cur.reserve(n_vocab);
    for (llama_token token_id = 0; token_id < (llama_token) n_vocab; token_id++) {
        cur.emplace_back(llama_token_data{ token_id, 0.0f, 0.0f });
    }
    auto tok_arr = llama_token_data_array{ cur.data(), cur.size(), -1, false };
    llama_sampler_apply(smpl, &tok_arr);

    auto tok_eos = llama_vocab_eot(vocab);
    if (tok_eos == LLAMA_TOKEN_NULL) {
        tok_eos = llama_vocab_eos(vocab);
    }
    return cur[tok_eos].logit >= 0.0f;
}

// True iff the grammar is currently constraining (some token is masked out).
static bool is_constraining(llama_sampler * smpl) {
    const auto n_vocab = llama_vocab_n_tokens(vocab);
    std::vector<llama_token_data> cur;
    cur.reserve(n_vocab);
    for (llama_token token_id = 0; token_id < (llama_token) n_vocab; token_id++) {
        cur.emplace_back(llama_token_data{ token_id, 0.0f, 0.0f });
    }
    auto tok_arr = llama_token_data_array{ cur.data(), cur.size(), -1, false };
    llama_sampler_apply(smpl, &tok_arr);
    for (size_t i = 0; i < cur.size(); i++) {
        if (cur[i].logit < 0.0f) {
            return true;
        }
    }
    return false;
}

static const char * TOOL_GRAMMAR =
    "root ::= \"<tool_call>get_time(\\\"\" [A-Za-z]+ \"\\\")</tool_call>\"";

int main(int argc, char ** argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <vocab-file>\n", argv[0]);
        return 1;
    }

    llama_backend_init();

    llama_model_params mparams = llama_model_default_params();
    mparams.vocab_only = true;
    llama_model * model = llama_model_load_from_file(argv[1], mparams);
    assert(model != nullptr);
    vocab = llama_model_get_vocab(model);

    // 1. Recitation of the trigger marker in prose must not abort, must not
    //    permanently arm, and a genuine call afterwards must still be
    //    constrained and parse to completion.
    {
        llama_sampler * smpl = make_lazy_sampler(TOOL_GRAMMAR, "<tool_call>");

        // The `">` right after the marker cannot parse; pre-fix this threw
        // "Unexpected empty grammar stack" and the server returned HTTP 500.
        assert(accept_string(smpl, "You can invoke tools by writing a \"<tool_call>\" block. "));
        // Not armed: free text still unconstrained.
        assert(!is_constraining(smpl));

        // Genuine call: the second occurrence arms and constrains.
        assert(accept_string(smpl, "<tool_call>get_ti"));
        // The genuine occurrence must actually arm the grammar (discriminates
        // against "disarm permanently on first failure" strategies).
        assert(is_constraining(smpl));
        assert(accept_string(smpl, "me(\"Tokyo\")</tool_call>"));
        assert(eog_allowed(smpl));

        llama_sampler_free(smpl);
    }

    // 2. Recitation containing a placeholder example (like the DS4-Flash
    //    header's $TOOL_NAME) rolls back on the name rule, then a real call
    //    arms.
    {
        llama_sampler * smpl = make_lazy_sampler(TOOL_GRAMMAR, "<tool_call>");

        assert(accept_string(smpl, "Example: <tool_call>$TOOL_NAME($ARGS)</tool_call> — like that.\n"));
        assert(!is_constraining(smpl));

        assert(accept_string(smpl, "<tool_call>get_ti"));
        // The genuine occurrence must actually arm the grammar (discriminates
        // against "disarm permanently on first failure" strategies).
        assert(is_constraining(smpl));
        assert(accept_string(smpl, "me(\"Paris\")</tool_call>"));
        assert(eog_allowed(smpl));

        llama_sampler_free(smpl);
    }

    // 3. A quoted marker where the continuation is a *prefix* of a valid call
    //    but then diverges: replay succeeds at arm time (the buffered suffix
    //    parses so far), and the grammar then constrains the continuation —
    //    "get_tim" + "ber" would be rejected mid-word by apply(). Here we
    //    check the plain direct-call path for completeness.
    {
        llama_sampler * smpl = make_lazy_sampler(TOOL_GRAMMAR, "<tool_call>");

        assert(accept_string(smpl, "<tool_call>get_time(\"NYC\")</tool_call>"));
        assert(eog_allowed(smpl));

        llama_sampler_free(smpl);
    }

    // 4. Multiple failed occurrences before the genuine one. Each quoted marker
    //    is immediately followed by a non-parsing character that BPE merges
    //    into the same piece as the closing `>`, so the occurrence is
    //    falsifiable at arm time.
    {
        llama_sampler * smpl = make_lazy_sampler(TOOL_GRAMMAR, "<tool_call>");

        assert(accept_string(smpl, "Write \"<tool_call>\" to call, yes \"<tool_call>\" exactly. "));
        assert(!is_constraining(smpl));

        assert(accept_string(smpl, "<tool_call>get_ti"));
        // The genuine occurrence must actually arm the grammar (discriminates
        // against "disarm permanently on first failure" strategies).
        assert(is_constraining(smpl));
        assert(accept_string(smpl, "me(\"Lima\")</tool_call>"));
        assert(eog_allowed(smpl));

        llama_sampler_free(smpl);
    }

    // 4b. Residual (documented, pre-existing): a marker whose continuation is
    //     not yet in the buffer arms immediately — there is nothing to falsify
    //     yet, and deferring the arm would un-constrain the first payload token
    //     of genuine calls. If the marker was actually quoted prose, the model
    //     is then constrained mid-prose (no crash; prose tokens get masked).
    //     In practice this is rare: BPE merges the closing `>` with adjacent
    //     punctuation (`>"`, `>:`, …), which lands the falsifying byte in the
    //     buffer at arm time (cases 1, 2, 4 above).
    {
        llama_sampler * smpl = make_lazy_sampler(TOOL_GRAMMAR, "<tool_call>");

        assert(accept_string(smpl, "Not <tool_call>"));
        assert(is_constraining(smpl));

        llama_sampler_free(smpl);
    }

    // 5. After sampler reset, the scan offset must be back to zero: a genuine
    //    call at the very start arms.
    {
        llama_sampler * smpl = make_lazy_sampler(TOOL_GRAMMAR, "<tool_call>");

        assert(accept_string(smpl, "Quote: \"<tool_call>\" end. "));
        llama_sampler_reset(smpl);
        assert(accept_string(smpl, "<tool_call>get_time(\"Oslo\")</tool_call>"));
        assert(eog_allowed(smpl));

        llama_sampler_free(smpl);
    }

    llama_model_free(model);
    llama_backend_free();

    fprintf(stderr, "All lazy-trigger tests passed.\n");
    return 0;
}
