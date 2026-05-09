// vlib_video_encoder.h — pluggable per-frame encoder for the video session.
//
// The session never calls `ggml_conv_3d` or the mtmd graph directly; it asks
// an encoder for `(ref_frame, cur_frame) -> embedding tokens`.
//
// Two impls (see VIDEO_CONV3D_CPP_DESIGN.md §3.2 / §3.3):
//   - VLIB_ENCODE_MODE_PAIR2D : runs the existing two-Conv2D patch heads
//                               with two distinct frames (ref into _0, cur
//                               into _1). Mathematically equivalent to a
//                               t=2/stride=2 Conv3D when the GGUF stores
//                               the kernel split along KT (which it does
//                               for Qwen2/2.5/3-VL).
//   - VLIB_ENCODE_MODE_CONV3D : packs patch_embeddings_0/_1 into a 5D
//                               tensor at clip load and runs ggml_conv_3d.
//                               Mainline-only (CPU + Metal).
//
// In v1 the conv3d mode is gated behind a runtime flag and falls back to
// pair2d on backends without `ggml_conv_3d` (Vulkan/CUDA/SYCL/HIP).
//
// We also expose a STUB encoder for unit tests (S3, S9) that produces a
// zero-embedding of the right shape so the session state machine can be
// exercised without a real model.

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

struct clip_ctx;
struct llama_context;
struct mtmd_context;

namespace vlib {

enum encode_mode {
    ENCODE_MODE_STUB   = 0, // zero embedding, for unit tests
    ENCODE_MODE_PAIR2D = 1, // two Conv2D heads with distinct ref/cur frames
    ENCODE_MODE_CONV3D = 2, // single Conv3D over [W,H,T=2,C=3]
};

// Per-frame MRoPE position descriptor. Mirrors mtmd_decoder_pos but kept
// in the vlib namespace so we can compile the unit tests without dragging
// in clip-graph internals.
struct decoder_pos {
    uint32_t t;
    uint32_t x;
    uint32_t y;
    uint32_t z; // reserved
};

// Encoder interface. Implementations are reference-only — they do not own
// the clip_ctx/llama_context/mtmd_context handles passed at construction.
struct video_encoder {
    virtual ~video_encoder() = default;

    // Preprocessed frame width/height in pixels. For Qwen3-VL the mtmd image
    // preproc resizes incoming frames to a multiple of patch_size*2 so we
    // pin nx/ny at session-start.
    virtual int32_t nx() const = 0;
    virtual int32_t ny() const = 0;

    // Number of mmproj-projected embedding tokens emitted by one
    // (ref, cur) pair. For Qwen3-VL with a 504x392 frame this is
    // (nx/14/2)*(ny/14/2) = 252.
    virtual int32_t n_tokens_per_pair() const = 0;

    // Embedding dimension (== llama_model_n_embd_inp(text_model)).
    virtual int32_t n_embd() const = 0;

    // Encode a (reference, current) frame pair.
    //
    // frame_ref / frame_cur : RGB f32, NHWC, length nx*ny*3 each.
    // out_embd              : pre-allocated, n_tokens_per_pair() * n_embd().
    // out_n_tokens          : (output) actual token count, normally ==
    //                         n_tokens_per_pair() but the encoder is allowed
    //                         to emit fewer for partial frames.
    // out_pos               : (output) per-token MRoPE position. Caller
    //                         provides a buffer of length n_tokens_per_pair().
    //
    // Returns 0 on success, non-zero on failure.
    virtual int32_t encode_pair(const float * frame_ref,
                                const float * frame_cur,
                                float       * out_embd,
                                int32_t     * out_n_tokens,
                                decoder_pos * out_pos) = 0;
};

// Stub encoder: zero embeddings, no clip_ctx required. Used by U1/U3/U4.
std::unique_ptr<video_encoder> make_stub_encoder(int32_t nx,
                                                 int32_t ny,
                                                 int32_t patch_size,
                                                 int32_t n_embd,
                                                 int32_t n_merge = 2);

// Pair2D encoder: feeds frames through the existing two-Conv2D heads via
// mtmd_encode_video_pair (added in S4). Returns nullptr if mtmd_ctx is
// missing vision support or its projector is not a Qwen-VL family head.
std::unique_ptr<video_encoder> make_pair2d_encoder(mtmd_context * mtmd_ctx,
                                                   int32_t        nx,
                                                   int32_t        ny);

// Conv3D encoder: requires a clip_ctx with patch_embeddings_0/_1 packed
// into a 5D tensor at load time (see clip.cpp QWEN3VL branch). Falls back
// to pair2d on backends without ggml_conv_3d. Default off in v1.
std::unique_ptr<video_encoder> make_conv3d_encoder(mtmd_context * mtmd_ctx,
                                                   int32_t        nx,
                                                   int32_t        ny);

} // namespace vlib
