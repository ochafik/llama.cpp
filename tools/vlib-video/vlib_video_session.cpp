// vlib_video_session.cpp — continuous-session state machine.
//
// State machine (per VIDEO_CONV3D_CPP_DESIGN.md §4.3):
//
//   process_frame(cur_rgb, audio_text):
//     1. frame_count++
//     2. if (audio_text):
//          decode "\n[Audio transcript]: ..." text tokens at n_past
//     3. rewind_floor = n_past   (NB: pinned ABOVE sys_pos_end)
//     4. ref = ref_set ? ref_frame : cur_frame
//     5. encoder.encode_pair(ref, cur, &embd, &n_vis_tokens, &mrope_pos)
//     6. decode chunk_text(open) ; decode embedding chunk ; decode chunk_text(close)
//     7. generate up to max_tool_tokens; stop on </tool_call>, EOS, or budget
//     8. parse generated text -> tool_call
//     9. if action == do_nothing/ignore_frame:
//          llama_memory_seq_rm(rewind_floor, -1) ; n_past = rewind_floor ;
//          pop the cumulative grid_thw entry
//        else (speak / note / unknown):
//          ref_frame = cur_frame ; ref_set = true
//
// MRoPE positions: each kept frame contributes one (t=1, h, w) entry to a
// running cumulative list. The session emits per-token mrope positions with
// pos_0_t = sum-of-prev-frame-t (i.e. cumulative video time across kept
// frames only, matching MLX's _position_ids recompute on rewind).

#include "vlib_video_session.h"
#include "mtmd.h"

#include "llama.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace vlib {

namespace {

std::string fmt(const char * f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return std::string(buf);
}

class session_impl : public session {
public:
    session_impl(llama_context * lctx,
                 const llama_model * model,
                 mtmd_context  * ctx_mtmd,
                 video_encoder * enc,
                 const session_params & params)
        : m_lctx(lctx),
          m_model(model),
          m_mtmd(ctx_mtmd),
          m_enc(enc),
          m_params(params),
          m_n_past(0),
          m_sys_pos_end(0),
          m_frame_count(0),
          m_seq_id(params.seq_id),
          m_ref_set(false) {}

    int32_t start() override {
        if (m_params.system_prompt.empty()) {
            m_sys_pos_end = 0;
            return 0;
        }
        std::vector<llama_token> toks = tokenize(m_params.system_prompt, /*add_special=*/true, /*parse_special=*/true);
        if (toks.empty()) return -1;
        if (decode_text_tokens(toks) != 0) return -2;
        m_sys_pos_end = m_n_past;
        return 0;
    }

    action process_frame(const float * frame_rgb_data, const std::string & audio_text) override {
        action result;
        m_frame_count++;

        // Step 2: optional audio transcript chunk (persists across rewinds).
        if (!audio_text.empty()) {
            std::string s = fmt("\n[Audio transcript]: %s\n", audio_text.c_str());
            std::vector<llama_token> toks = tokenize(s, /*add_special=*/false, /*parse_special=*/false);
            if (!toks.empty() && decode_text_tokens(toks) != 0) {
                return result;
            }
        }

        // Step 3: snapshot rewind floor.
        const llama_pos rewind_floor = m_n_past;
        m_pending_rewind_floor = rewind_floor;
        m_pending_pop_grid     = false;

        // Step 4: reference frame selection.
        const float * ref = m_ref_set ? m_ref_frame.data() : frame_rgb_data;

        // Step 5: encode pair via the injected encoder.
        const int32_t n_tok_pair = m_enc->n_tokens_per_pair();
        const int32_t n_embd     = m_enc->n_embd();
        m_embd_scratch.assign((size_t)n_tok_pair * (size_t)n_embd, 0.0f);
        m_pos_scratch.assign((size_t)n_tok_pair, decoder_pos{});
        int32_t n_vis = 0;
        int rc = m_enc->encode_pair(ref, frame_rgb_data,
                                    m_embd_scratch.data(),
                                    &n_vis, m_pos_scratch.data());
        if (rc != 0) {
            return result;
        }
        if (n_vis <= 0) {
            return result;
        }

        // Step 5b: push cumulative grid_thw for MRoPE position math.
        const int32_t patch_size = 14;
        const int32_t merge      = 2;
        grid_entry ge;
        ge.t = 1;
        ge.h = (uint32_t)(m_enc->ny() / patch_size / merge);
        ge.w = (uint32_t)(m_enc->nx() / patch_size / merge);
        m_grid_thw.push_back(ge);
        m_pending_pop_grid = true;

        // Step 6: surround visual chunk with chat-template tokens.
        std::string label = fmt(m_params.per_frame_label_format.c_str(), (int)m_frame_count);
        std::string opener = m_params.per_frame_user_prefix + label + m_params.vision_open;
        std::string closer = m_params.vision_close + m_params.per_frame_user_suffix;

        std::vector<llama_token> open_toks  = tokenize(opener, /*add_special=*/false, /*parse_special=*/true);
        std::vector<llama_token> close_toks = tokenize(closer, /*add_special=*/false, /*parse_special=*/true);

        if (decode_text_tokens(open_toks) != 0)            { return result; }
        if (decode_video_embd(m_embd_scratch.data(), n_vis) != 0) { return result; }
        if (decode_text_tokens(close_toks) != 0)           { return result; }

        // Step 7-8: generate until </tool_call>, EOS, or budget.
        std::string assistant_text = generate_until_stop(m_params.max_tool_tokens);
        result.raw_assistant_text = assistant_text;

        // Step 9: parse + dispatch.
        tool_call tc;
        bool parsed = parse_tool_call(assistant_text, tc);
        if (!parsed) {
            // No parseable tool call — treat like an unknown action: keep KV.
            result.kind = ACTION_NONE;
            consume_frame(frame_rgb_data);
            return result;
        }
        result.call = tc;

        const std::string & name = tc.name;
        const bool is_ignore = (name == "do_nothing" || name == "ignore_frame");
        if (is_ignore) {
            // Selective rewind. Recurrent state is intentionally NOT rolled
            // back (matches MLX ghost-memory semantic).
            llama_memory_t mem = llama_get_memory(m_lctx);
            if (mem) {
                llama_memory_seq_rm(mem, m_seq_id, rewind_floor, -1);
            }
            m_n_past = rewind_floor;
            // Pop the cumulative grid entry we pushed for this frame.
            if (!m_grid_thw.empty()) {
                m_grid_thw.pop_back();
            }
            m_pending_pop_grid = false;
            result.kind = ACTION_DO_NOTHING;
            return result;
        }

        // Speak / note / other: cur becomes the new ref; n_past stays advanced.
        consume_frame(frame_rgb_data);
        if (name == "speak") {
            result.kind = ACTION_SPEAK;
            if (m_speak_cb) {
                auto it = tc.arguments.find("text");
                m_speak_cb(it != tc.arguments.end() ? it->second : std::string{}, m_speak_user);
            }
        } else if (name == "note") {
            result.kind = ACTION_NOTE;
            if (m_note_cb) {
                auto it = tc.arguments.find("observation");
                m_note_cb(it != tc.arguments.end() ? it->second : std::string{}, m_note_user);
            }
        } else {
            result.kind = ACTION_OTHER;
        }
        return result;
    }

    void rewind_last() override {
        if (m_pending_rewind_floor < 0) return;
        llama_memory_t mem = llama_get_memory(m_lctx);
        if (mem) {
            llama_memory_seq_rm(mem, m_seq_id, m_pending_rewind_floor, -1);
        }
        m_n_past = m_pending_rewind_floor;
        if (m_pending_pop_grid && !m_grid_thw.empty()) {
            m_grid_thw.pop_back();
        }
        m_pending_pop_grid = false;
        m_pending_rewind_floor = -1;
    }

    void set_speak_cb(speak_cb cb, void * cb_user) override { m_speak_cb = std::move(cb); m_speak_user = cb_user; }
    void set_note_cb (note_cb  cb, void * cb_user) override { m_note_cb  = std::move(cb); m_note_user  = cb_user; }

    llama_pos n_past() const override { return m_n_past; }
    int32_t   frame_count() const override { return m_frame_count; }
    size_t    cumulative_grid_size() const override { return m_grid_thw.size(); }

    // Test hook — exposed via the public surface for U3's MRoPE assertion.
    // Returns the cumulative t-sum across all kept frames.
    uint32_t cumulative_t_sum() const {
        uint32_t s = 0;
        for (const auto & e : m_grid_thw) s += e.t;
        return s;
    }

private:
    struct grid_entry { uint32_t t; uint32_t h; uint32_t w; };

    void consume_frame(const float * frame_rgb_data) {
        if (m_enc == nullptr) return;
        const size_t need = (size_t)m_enc->nx() * (size_t)m_enc->ny() * 3;
        m_ref_frame.assign(frame_rgb_data, frame_rgb_data + need);
        m_ref_set = true;
    }

    std::vector<llama_token> tokenize(const std::string & text, bool add_special, bool parse_special) {
        if (m_model == nullptr) return {};
        const llama_vocab * vocab = llama_model_get_vocab(m_model);
        if (!vocab) return {};

        // Two-pass: query needed size, then materialize.
        int32_t need = -llama_tokenize(vocab, text.c_str(), (int32_t)text.size(),
                                       nullptr, 0, add_special, parse_special);
        if (need <= 0) return {};
        std::vector<llama_token> out(need);
        int32_t got = llama_tokenize(vocab, text.c_str(), (int32_t)text.size(),
                                     out.data(), (int32_t)out.size(),
                                     add_special, parse_special);
        if (got < 0) return {};
        out.resize(got);
        return out;
    }

    int decode_text_tokens(const std::vector<llama_token> & toks) {
        if (toks.empty()) return 0;
        // Build a batch piecewise so we don't exceed n_batch.
        const int32_t n_batch = (int32_t)std::min<uint32_t>(512u, (uint32_t)toks.size());
        for (size_t off = 0; off < toks.size(); off += (size_t)n_batch) {
            int32_t this_batch = (int32_t)std::min<size_t>((size_t)n_batch, toks.size() - off);
            llama_batch b = llama_batch_init(this_batch, 0, 1);
            for (int32_t i = 0; i < this_batch; ++i) {
                b.token   [b.n_tokens] = toks[off + i];
                b.pos     [b.n_tokens] = m_n_past + (int32_t)off + i;
                b.n_seq_id[b.n_tokens] = 1;
                b.seq_id  [b.n_tokens][0] = m_seq_id;
                // Only the very last token across the whole sequence emits logits.
                bool emit = (off + (size_t)i + 1 == toks.size());
                b.logits  [b.n_tokens] = emit ? 1 : 0;
                b.n_tokens++;
            }
            int rc = llama_decode(m_lctx, b);
            llama_batch_free(b);
            if (rc != 0) return rc;
        }
        m_n_past += (llama_pos)toks.size();
        return 0;
    }

    int decode_video_embd(const float * /*embd*/, int32_t n_vis_tokens) {
        // In v1 we delegate the actual embedding-decode to mtmd_helper —
        // hooked up in S5 once mtmd has a VIDEO chunk. For S3 we treat the
        // visual chunk as a positional advance, which is enough to drive
        // the U3/U4 unit tests against the stub encoder (those tests
        // assert n_past arithmetic, not the actual embedding bytes).
        m_n_past += (llama_pos)n_vis_tokens;
        return 0;
    }

    std::string generate_until_stop(int32_t max_tokens) {
        // Minimal greedy generator using the last logits row.
        // Stops on EOS, on a generated </tool_call>, or after max_tokens.
        if (m_model == nullptr) return {};
        const llama_vocab * vocab = llama_model_get_vocab(m_model);
        if (!vocab) return {};
        std::string out;
        const llama_token tok_eos = llama_vocab_eos(vocab);

        for (int32_t i = 0; i < max_tokens; ++i) {
            float * logits = llama_get_logits_ith(m_lctx, -1);
            if (!logits) break;
            const int32_t n_vocab = llama_vocab_n_tokens(vocab);

            // Greedy argmax.
            llama_token best = 0;
            float best_v = logits[0];
            for (int32_t t = 1; t < n_vocab; ++t) {
                if (logits[t] > best_v) { best_v = logits[t]; best = (llama_token)t; }
            }
            if (best == tok_eos) break;

            // Convert to piece and append.
            char buf[64];
            int32_t n = llama_token_to_piece(vocab, best, buf, sizeof(buf), /*lstrip=*/0, /*special=*/true);
            if (n > 0) out.append(buf, buf + n);

            // Decode this single token to advance state.
            llama_batch b = llama_batch_init(1, 0, 1);
            b.token   [0] = best;
            b.pos     [0] = m_n_past;
            b.n_seq_id[0] = 1;
            b.seq_id  [0][0] = m_seq_id;
            b.logits  [0] = 1;
            b.n_tokens = 1;
            int rc = llama_decode(m_lctx, b);
            llama_batch_free(b);
            if (rc != 0) break;
            m_n_past++;

            // Stop if we just closed a tool call.
            if (out.find("</tool_call>") != std::string::npos) break;
        }
        return out;
    }

    llama_context     * m_lctx;
    const llama_model * m_model;
    mtmd_context      * m_mtmd;
    video_encoder     * m_enc;
    session_params      m_params;

    llama_pos     m_n_past;
    llama_pos     m_sys_pos_end;
    int32_t       m_frame_count;
    llama_seq_id  m_seq_id;

    bool                   m_ref_set;
    std::vector<float>     m_ref_frame;

    std::vector<float>           m_embd_scratch;
    std::vector<decoder_pos>     m_pos_scratch;

    std::vector<grid_entry>      m_grid_thw;
    llama_pos                    m_pending_rewind_floor = -1;
    bool                         m_pending_pop_grid     = false;

    speak_cb m_speak_cb;
    void *   m_speak_user = nullptr;
    note_cb  m_note_cb;
    void *   m_note_user  = nullptr;
};

} // namespace

std::unique_ptr<session> session::create(llama_context * lctx,
                                         const llama_model * model,
                                         mtmd_context  * ctx_mtmd,
                                         video_encoder * enc,
                                         const session_params & params) {
    return std::unique_ptr<session>(new session_impl(lctx, model, ctx_mtmd, enc, params));
}

} // namespace vlib
