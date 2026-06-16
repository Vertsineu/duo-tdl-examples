#ifndef TDL_V2_DRAW_H_
#define TDL_V2_DRAW_H_

#include <stdbool.h>
#include "cvi_comm.h"
#include "tdl_types.h"

#ifdef __cplusplus
extern "C" {
#endif

TDLBrush SAMPLE_TDL_DefaultBrush(void);
CVI_S32 SAMPLE_TDL_DrawObjectRect(const TDLObject *meta, VIDEO_FRAME_INFO_S *frame,
                                  bool draw_text, TDLBrush brush);
CVI_S32 SAMPLE_TDL_DrawFaceRect(const TDLFace *meta, VIDEO_FRAME_INFO_S *frame, bool draw_text,
                                TDLBrush brush);

#ifdef __cplusplus
}
#endif

#endif
