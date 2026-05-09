// mtmd-video-cli — minimal driver for the vlib-video continuous session.
//
// We deliberately avoid OpenCV and any video container parsing — frames are
// read as JPEG/PNG files from a directory in alphabetical order (or piped in
// via the simple `--frames-dir` arg) and decoded via stb_image, which is
// already vendored under llama.cpp/vendor/stb/.
//
// Usage:
//   llama-mtmd-video-cli -m TEXT.gguf --mmproj MMPROJ.gguf \
//                        --frames-dir DIR \
//                        --fps 0.5 \
//                        --system-prompt "Security Monitor."
//
// Per-frame the driver:
//   1. resizes to 504x392 (default Qwen3-VL preproc target — stb resize),
//   2. converts to RGB f32 NHWC,
//   3. calls vlib::session::process_frame(),
//   4. prints the resulting action (do_nothing / speak / note / other).

#include "arg.h"
#include "common.h"
#include "log.h"
#include "llama.h"
#include "mtmd.h"

#include "vlib_video.h"

#define STB_IMAGE_IMPLEMENTATION
#include "vendor/stb/stb_image.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

struct cli_params {
    std::string frames_dir;
    std::string system_prompt;
    int   target_nx     = 504;
    int   target_ny     = 392;
    float fps           = 0.5f; // not used yet — frames are processed back-to-back
    int32_t max_frames  = 0;    // 0 = unbounded
};

void show_help(const char * argv0) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Required:\n"
        "  -m, --model FILE          text model GGUF\n"
        "      --mmproj FILE         vision mmproj GGUF\n"
        "      --frames-dir DIR      directory of jpeg/png frames (alphabetical order)\n"
        "\n"
        "Optional:\n"
        "      --system-prompt STR   system prompt (default: 'You are a continuous video analyser.')\n"
        "      --target-nx N         preprocess to this width (default 504)\n"
        "      --target-ny N         preprocess to this height (default 392)\n"
        "      --fps F               frame iteration fps (default 0.5; not yet enforced)\n"
        "      --max-frames N        process at most N frames (default: all)\n"
        , argv0);
}

bool list_frame_files(const std::string & dir, std::vector<std::string> & out) {
    DIR * d = opendir(dir.c_str());
    if (!d) return false;
    struct dirent * ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        if (name.size() > 4) {
            std::string lower = name;
            for (auto & c : lower) c = (char)tolower((unsigned char)c);
            bool ok =
                lower.find(".jpg")  != std::string::npos ||
                lower.find(".jpeg") != std::string::npos ||
                lower.find(".png")  != std::string::npos ||
                lower.find(".bmp")  != std::string::npos;
            if (!ok) continue;
        }
        out.push_back(dir + "/" + name);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return true;
}

// Decode + resize a frame to (nx, ny) RGB f32 NHWC, normalized to [0..1].
//
// We deliberately skip the model's own image preprocessor (mean/std,
// dynamic resize) here because vlib_video_session's encoder treats
// `frame_rgb_data` as already-preprocessed. The mtmd-helper bitmap path
// is the production-quality alternative once the VIDEO chunk dispatch
// graduates from "session calls mtmd_encode_video_pair directly".
bool load_and_preprocess(const std::string & path, int target_nx, int target_ny,
                         std::vector<float> & out_f32) {
    int w = 0, h = 0, ch = 0;
    unsigned char * data = stbi_load(path.c_str(), &w, &h, &ch, 3);
    if (!data) {
        LOG_ERR("failed to read %s\n", path.c_str());
        return false;
    }
    out_f32.assign((size_t)target_nx * target_ny * 3, 0.0f);
    // nearest-neighbour scale; sufficient for v1 (mtmd preproc is a TODO).
    for (int y = 0; y < target_ny; ++y) {
        int sy = (int)((int64_t)y * h / target_ny);
        for (int x = 0; x < target_nx; ++x) {
            int sx = (int)((int64_t)x * w / target_nx);
            unsigned char * p = &data[3 * (sy * w + sx)];
            float * o = &out_f32[3 * (y * target_nx + x)];
            o[0] = p[0] / 255.0f;
            o[1] = p[1] / 255.0f;
            o[2] = p[2] / 255.0f;
        }
    }
    stbi_image_free(data);
    return true;
}

const char * action_kind_name(vlib::action_kind k) {
    switch (k) {
        case vlib::ACTION_NONE:       return "none";
        case vlib::ACTION_DO_NOTHING: return "do_nothing";
        case vlib::ACTION_SPEAK:      return "speak";
        case vlib::ACTION_NOTE:       return "note";
        case vlib::ACTION_OTHER:      return "other";
    }
    return "?";
}

} // namespace

int main(int argc, char ** argv) {
    common_params params;
    cli_params vparams;
    vparams.system_prompt = "You are a continuous video analyser.";

    // Parse vlib-specific args; pass anything else to common_params_parse.
    std::vector<char *> rest;
    rest.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char * key, std::string & dst) -> bool {
            if (a == key) {
                if (i + 1 >= argc) { fprintf(stderr, "%s requires a value\n", key); exit(1); }
                dst = argv[++i];
                return true;
            }
            return false;
        };
        auto nexti = [&](const char * key, int & dst) -> bool {
            if (a == key) {
                if (i + 1 >= argc) { fprintf(stderr, "%s requires a value\n", key); exit(1); }
                dst = atoi(argv[++i]);
                return true;
            }
            return false;
        };
        auto nextf = [&](const char * key, float & dst) -> bool {
            if (a == key) {
                if (i + 1 >= argc) { fprintf(stderr, "%s requires a value\n", key); exit(1); }
                dst = (float)atof(argv[++i]);
                return true;
            }
            return false;
        };
        if (next  ("--frames-dir",    vparams.frames_dir))    continue;
        if (next  ("--system-prompt", vparams.system_prompt)) continue;
        if (nexti ("--target-nx",     vparams.target_nx))     continue;
        if (nexti ("--target-ny",     vparams.target_ny))     continue;
        if (nextf ("--fps",           vparams.fps))           continue;
        if (nexti ("--max-frames",    vparams.max_frames))    continue;
        if (a == "-h" || a == "--help") { show_help(argv[0]); return 0; }
        rest.push_back(argv[i]);
    }

    if (vparams.frames_dir.empty()) {
        show_help(argv[0]);
        fprintf(stderr, "\nERROR: --frames-dir is required\n");
        return 1;
    }

    if (!common_params_parse((int)rest.size(), rest.data(), params, LLAMA_EXAMPLE_MTMD)) {
        return 1;
    }

    common_init();
    llama_backend_init();
    llama_numa_init(params.numa);

    common_init_result_ptr llama_init = common_init_from_params(params);
    llama_model   * model = llama_init->model();
    llama_context * lctx  = llama_init->context();
    if (!model || !lctx) return 2;

    mtmd_context_params mparams = mtmd_context_params_default();
    mparams.use_gpu       = params.mmproj_use_gpu;
    mparams.n_threads     = params.cpuparams.n_threads;
    mparams.flash_attn_type = params.flash_attn_type;
    mparams.warmup        = params.warmup;
    mtmd_context * mtmd = mtmd_init_from_file(params.mmproj.path.c_str(), model, mparams);
    if (!mtmd) {
        LOG_ERR("failed to init mtmd from %s\n", params.mmproj.path.c_str());
        return 3;
    }

    auto enc = vlib::make_pair2d_encoder(mtmd, vparams.target_nx, vparams.target_ny);
    if (!enc) {
        LOG_ERR("encoder init failed (probably non-Qwen-VL projector)\n");
        mtmd_free(mtmd);
        return 4;
    }

    vlib::session_params sp;
    sp.system_prompt = vparams.system_prompt;
    auto sess = vlib::session::create(lctx, model, mtmd, enc.get(), sp);
    if (sess->start() != 0) {
        LOG_ERR("session start failed\n");
        mtmd_free(mtmd);
        return 5;
    }

    sess->set_speak_cb([](const std::string & text, void *) {
        printf("[SPEAK] %s\n", text.c_str());
    }, nullptr);
    sess->set_note_cb([](const std::string & obs, void *) {
        printf("[NOTE]  %s\n", obs.c_str());
    }, nullptr);

    std::vector<std::string> frame_paths;
    if (!list_frame_files(vparams.frames_dir, frame_paths)) {
        LOG_ERR("could not open frames dir %s\n", vparams.frames_dir.c_str());
        mtmd_free(mtmd);
        return 6;
    }
    if (vparams.max_frames > 0 && (int)frame_paths.size() > vparams.max_frames) {
        frame_paths.resize(vparams.max_frames);
    }
    LOG_INF("%s: %zu frames queued\n", __func__, frame_paths.size());

    std::vector<float> frame_buf;
    for (size_t i = 0; i < frame_paths.size(); ++i) {
        if (!load_and_preprocess(frame_paths[i], vparams.target_nx, vparams.target_ny, frame_buf)) {
            continue;
        }
        vlib::action a = sess->process_frame(frame_buf.data());
        printf("frame %zu/%zu: action=%s name=%s\n",
               i + 1, frame_paths.size(),
               action_kind_name(a.kind),
               a.call.name.c_str());
    }

    mtmd_free(mtmd);
    llama_backend_free();
    return 0;
}
