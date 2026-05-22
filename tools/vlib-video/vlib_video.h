// vlib_video.h — umbrella header for the vlib-video static lib.
//
// Public surface:
//   - vlib_video_tool_parser : <tool_call> parser (Qwen2.5 JSON + Qwen3.5 XML)
//   - vlib_video_encoder     : per-frame encoder (pair2d, conv3d-stub)
//   - vlib_video_session     : continuous-session state machine
//
// See VIDEO_CONV3D_CPP_DESIGN.md (sections 3, 4, 7.1) for the design.

#pragma once

#include "vlib_video_tool_parser.h"
#include "vlib_video_encoder.h"
#include "vlib_video_session.h"
#include "vlib_video_frame_filter.h"
