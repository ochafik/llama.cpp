// vlib_video_encoder.cpp — encoder factories.
//
// In S1 we ship only the STUB encoder (zero embeddings of the right shape).
// S4 fills in `make_pair2d_encoder` once `mtmd_encode_video_pair` lands.
// S6 fills in `make_conv3d_encoder` once the load-time pack is wired up.

#include "vlib_video_encoder.h"
#include "mtmd.h"

#include <cstring>
#include <vector>

namespace vlib {

namespace {

class stub_encoder : public video_encoder {
public:
    stub_encoder(int32_t nx, int32_t ny, int32_t patch_size, int32_t n_embd, int32_t n_merge)
        : m_nx(nx), m_ny(ny), m_patch_size(patch_size), m_n_embd(n_embd), m_n_merge(n_merge) {
        // n_tokens_per_pair: spatial-merge cells over the patch grid.
        // For Qwen3-VL with patch_size=14, n_merge=2 -> nx/14/2 * ny/14/2.
        // T_out is 1 (Conv3D collapses T=2 stride=2 to 1).
        const int32_t patches_x = m_nx / m_patch_size;
        const int32_t patches_y = m_ny / m_patch_size;
        const int32_t merged_x  = patches_x / m_n_merge;
        const int32_t merged_y  = patches_y / m_n_merge;
        m_n_tokens = merged_x * merged_y;
        m_grid_h   = merged_y;
        m_grid_w   = merged_x;
    }

    int32_t nx()                 const override { return m_nx; }
    int32_t ny()                 const override { return m_ny; }
    int32_t n_tokens_per_pair()  const override { return m_n_tokens; }
    int32_t n_embd()             const override { return m_n_embd; }

    int32_t encode_pair(const float * /*frame_ref*/,
                        const float * /*frame_cur*/,
                        float       * out_embd,
                        int32_t     * out_n_tokens,
                        decoder_pos * out_pos) override {
        if (out_embd) {
            std::memset(out_embd, 0, sizeof(float) * (size_t)m_n_tokens * (size_t)m_n_embd);
        }
        if (out_n_tokens) {
            *out_n_tokens = m_n_tokens;
        }
        if (out_pos) {
            // Row-major scan over the merged grid; t is set by the session
            // (cumulative video time) so we leave it at 0 here.
            int32_t i = 0;
            for (int32_t y = 0; y < m_grid_h; y++) {
                for (int32_t x = 0; x < m_grid_w; x++) {
                    out_pos[i++] = decoder_pos{ /*t=*/0, /*x=*/(uint32_t)x, /*y=*/(uint32_t)y, /*z=*/0 };
                }
            }
        }
        return 0;
    }

private:
    int32_t m_nx;
    int32_t m_ny;
    int32_t m_patch_size;
    int32_t m_n_embd;
    int32_t m_n_merge;
    int32_t m_n_tokens = 0;
    int32_t m_grid_h   = 0;
    int32_t m_grid_w   = 0;
};

} // namespace

std::unique_ptr<video_encoder> make_stub_encoder(int32_t nx,
                                                 int32_t ny,
                                                 int32_t patch_size,
                                                 int32_t n_embd,
                                                 int32_t n_merge) {
    return std::unique_ptr<video_encoder>(new stub_encoder(nx, ny, patch_size, n_embd, n_merge));
}

// ---------------------------------------------------------------------------
// Pair2D encoder — wraps mtmd_encode_video_pair (added in S4).
// ---------------------------------------------------------------------------

namespace {

class pair2d_encoder : public video_encoder {
public:
    pair2d_encoder(mtmd_context * ctx, int32_t nx, int32_t ny, int32_t patch_size, int32_t n_embd, int32_t n_merge)
        : m_ctx(ctx), m_nx(nx), m_ny(ny), m_patch_size(patch_size), m_n_embd(n_embd), m_n_merge(n_merge) {
        const int32_t patches_x = m_nx / m_patch_size;
        const int32_t patches_y = m_ny / m_patch_size;
        const int32_t merged_x  = patches_x / m_n_merge;
        const int32_t merged_y  = patches_y / m_n_merge;
        m_n_tokens = merged_x * merged_y;
        m_grid_h   = merged_y;
        m_grid_w   = merged_x;
    }

    int32_t nx()                 const override { return m_nx; }
    int32_t ny()                 const override { return m_ny; }
    int32_t n_tokens_per_pair()  const override { return m_n_tokens; }
    int32_t n_embd()             const override { return m_n_embd; }

    int32_t encode_pair(const float * frame_ref,
                        const float * frame_cur,
                        float       * out_embd,
                        int32_t     * out_n_tokens,
                        decoder_pos * out_pos) override {
        // S4 plumbing: ask mtmd for a pair encode. The mtmd entry point
        // builds a 2-image clip_image_f32_batch under the hood and routes
        // ref into inp_raw_ref / cur into inp_raw_cur.
        int rc = mtmd_encode_video_pair(m_ctx, m_nx, m_ny,
                                        frame_ref, frame_cur,
                                        out_embd);
        if (rc != 0) {
            return rc;
        }
        if (out_n_tokens) {
            *out_n_tokens = m_n_tokens;
        }
        if (out_pos) {
            int32_t i = 0;
            for (int32_t y = 0; y < m_grid_h; y++) {
                for (int32_t x = 0; x < m_grid_w; x++) {
                    out_pos[i++] = decoder_pos{ /*t=*/0, /*x=*/(uint32_t)x, /*y=*/(uint32_t)y, /*z=*/0 };
                }
            }
        }
        return 0;
    }

private:
    mtmd_context * m_ctx;
    int32_t m_nx;
    int32_t m_ny;
    int32_t m_patch_size;
    int32_t m_n_embd;
    int32_t m_n_merge;
    int32_t m_n_tokens = 0;
    int32_t m_grid_h   = 0;
    int32_t m_grid_w   = 0;
};

} // namespace

std::unique_ptr<video_encoder> make_pair2d_encoder(mtmd_context * mtmd_ctx,
                                                   int32_t        nx,
                                                   int32_t        ny) {
    if (mtmd_ctx == nullptr) {
        return nullptr;
    }
    if (!mtmd_support_vision(mtmd_ctx)) {
        return nullptr;
    }
    // Defaults match Qwen3-VL — a more careful impl would query mtmd for
    // patch_size/n_embd/n_merge from the loaded clip_ctx. We accept the
    // hard-coded values in v1 and fix it up in S6.
    const int32_t patch_size = 14;
    const int32_t n_embd     = 1152;
    const int32_t n_merge    = 2;
    return std::unique_ptr<video_encoder>(new pair2d_encoder(mtmd_ctx, nx, ny, patch_size, n_embd, n_merge));
}

std::unique_ptr<video_encoder> make_conv3d_encoder(mtmd_context * mtmd_ctx,
                                                   int32_t        nx,
                                                   int32_t        ny) {
    // S6 will land the load-time pack; for now we transparently fall back
    // to pair2d so callers can flip the runtime flag without breaking.
    return make_pair2d_encoder(mtmd_ctx, nx, ny);
}

} // namespace vlib
