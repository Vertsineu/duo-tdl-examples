#pragma once

#include <cvi_comm.h>
#include <tdl_types.h>

namespace cvi_media {
namespace tdl {

TDLBrush default_brush() noexcept;
CVI_S32 draw_object_rect(const TDLObject &meta, VIDEO_FRAME_INFO_S &frame, bool draw_text,
                         TDLBrush brush = default_brush()) noexcept;
CVI_S32 draw_face_rect(const TDLFace &meta, VIDEO_FRAME_INFO_S &frame, bool draw_text,
                       TDLBrush brush = default_brush()) noexcept;

}  // namespace tdl
}  // namespace cvi_media
