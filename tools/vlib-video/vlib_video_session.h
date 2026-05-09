// vlib_video_session.h — continuous video session state machine.
//
// Mirrors mlx_vlm/continuous_analyzer.py:ContinuousSession contract closely
// enough that recordings from one can be replayed against the other:
//
//   - Per-frame: encode (ref, cur) pair, build chunk list, decode, generate
//     until </tool_call> or EOS, parse, dispatch action.
//   - On do_nothing/ignore_frame: rewind KV cache to the pre-frame mark.
//     For hybrid models the recurrent state is intentionally NOT rolled back
//     (matches MLX "ghost memory" semantic — see CONTINUOUS.md:34).
//   - On speak/note: cur becomes the new ref; n_past advances.
//
// The session does not own any of the inference handles — caller hands in
// llama_context, mtmd_context, and an encoder, and is responsible for
// freeing them after the session is destroyed.

#pragma once

#include "vlib_video_tool_parser.h"
#include "vlib_video_encoder.h"

#include "llama.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct mtmd_context;

namespace vlib {

// Action returned by process_frame.
enum action_kind {
    ACTION_NONE      = 0, // parser failed; session held current frame
    ACTION_DO_NOTHING = 1, // canonical "no signal" — caused a rewind
    ACTION_SPEAK     = 2,
    ACTION_NOTE      = 3,
    ACTION_OTHER     = 4, // a tool name we don't recognize (still kept in KV)
};

struct action {
    action_kind kind = ACTION_NONE;
    tool_call   call;
    std::string raw_assistant_text; // verbatim model output for this frame
};

// Callback fired when the session decides to speak or note. cb_user is the
// opaque user pointer passed at session construction.
using speak_cb = std::function<void(const std::string & text, void * cb_user)>;
using note_cb  = std::function<void(const std::string & observation, void * cb_user)>;

struct session_params {
    // Sequence id to use for KV cache writes. Default 0.
    llama_seq_id seq_id = 0;

    // Generation cap per frame. Defaults to 256 — typical tool-call output
    // is < 64 tokens; this is the hard ceiling.
    int32_t max_tool_tokens = 256;

    // Sampling. We keep this minimal in v1: greedy by default.
    float temperature = 0.0f;
    int32_t top_k = 0;
    float top_p = 1.0f;

    // System prompt prepended once at session_start. Tokens emitted from
    // this prompt are pinned — rewinds never go below sys_pos_end.
    std::string system_prompt;

    // Per-frame chat-template fragments. We hard-code Qwen3-VL's chat
    // template strings here in v1; revisit when wiring to tools/server.
    std::string per_frame_user_prefix  = "\n<|im_start|>user\n";
    std::string per_frame_user_suffix  = "<|im_end|>\n<|im_start|>assistant\n";
    std::string vision_open            = "<|vision_start|>";
    std::string vision_close           = "<|vision_end|>";
    std::string per_frame_label_format = "[Frame %d] Analyze and call a tool.\n"; // %d -> frame index
};

struct session {
    // Construct a session bound to live llama/mtmd handles + an encoder.
    // The session does NOT take ownership of any of these pointers.
    static std::unique_ptr<session> create(llama_context * lctx,
                                           const llama_model * model,
                                           mtmd_context  * ctx_mtmd,
                                           video_encoder * enc,
                                           const session_params & params);

    virtual ~session() = default;

    // One-time bootstrap: tokenize and decode the system prompt.
    // Returns 0 on success, -1 on tokenization failure, -2 on decode failure.
    virtual int32_t start() = 0;

    // Process one frame. frame_rgb_data is RGB f32 NHWC of length nx*ny*3
    // where (nx, ny) match the encoder. audio_text_optional is appended as
    // a `\n[Audio transcript]: ...\n` text chunk before the visual chunk
    // and persists across rewinds (matches MLX behaviour).
    virtual action process_frame(const float * frame_rgb_data,
                                 const std::string & audio_text_optional = {}) = 0;

    // Direct rewind hook. Trims KV to the most recent pre-frame mark and
    // restores n_past. Recurrent state is intentionally NOT rolled back.
    // No-op if no frame has been processed yet.
    virtual void rewind_last() = 0;

    // Optional callbacks. Set to nullptr to skip. cb_user is opaque.
    virtual void set_speak_cb(speak_cb cb, void * cb_user) = 0;
    virtual void set_note_cb (note_cb  cb, void * cb_user) = 0;

    // Telemetry.
    virtual llama_pos n_past() const = 0;
    virtual int32_t   frame_count() const = 0;
    virtual size_t    cumulative_grid_size() const = 0; // for U3
};

} // namespace vlib
