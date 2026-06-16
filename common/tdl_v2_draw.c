#include "tdl_v2_draw.h"

#include <stdint.h>
#include <string.h>

#define DEFAULT_RECT_COLOR_R 53
#define DEFAULT_RECT_COLOR_G 208
#define DEFAULT_RECT_COLOR_B 217

static CVI_S32 clamp_i32(CVI_S32 value, CVI_S32 min_value, CVI_S32 max_value) {
  if (value < min_value) {
    return min_value;
  }
  if (value > max_value) {
    return max_value;
  }
  return value;
}

static CVI_U8 rgb_to_y(const color_rgb *color) {
  float y = 0.257f * color->r + 0.504f * color->g + 0.098f * color->b + 16.0f;
  return (CVI_U8)clamp_i32((CVI_S32)y, 0, 255);
}

static CVI_U8 rgb_to_u(const color_rgb *color) {
  float u = -0.148f * color->r - 0.291f * color->g + 0.439f * color->b + 128.0f;
  return (CVI_U8)clamp_i32((CVI_S32)u, 0, 255);
}

static CVI_U8 rgb_to_v(const color_rgb *color) {
  float v = 0.439f * color->r - 0.368f * color->g - 0.071f * color->b + 128.0f;
  return (CVI_U8)clamp_i32((CVI_S32)v, 0, 255);
}

static void fill_y_rect(CVI_U8 *plane, CVI_U32 stride, CVI_S32 width, CVI_S32 height, CVI_S32 x,
                        CVI_S32 y, CVI_S32 rect_w, CVI_S32 rect_h, CVI_U8 color) {
  CVI_S32 x1 = clamp_i32(x, 0, width - 1);
  CVI_S32 y1 = clamp_i32(y, 0, height - 1);
  CVI_S32 x2 = clamp_i32(x + rect_w - 1, 0, width - 1);
  CVI_S32 y2 = clamp_i32(y + rect_h - 1, 0, height - 1);

  if (x2 < x1 || y2 < y1) {
    return;
  }

  for (CVI_S32 row = y1; row <= y2; ++row) {
    memset(plane + row * stride + x1, color, x2 - x1 + 1);
  }
}

static void fill_uv_rect(CVI_U8 *plane, CVI_U32 stride, CVI_S32 width, CVI_S32 height,
                         CVI_S32 x, CVI_S32 y, CVI_S32 rect_w, CVI_S32 rect_h, CVI_U8 u,
                         CVI_U8 v, PIXEL_FORMAT_E fmt) {
  CVI_S32 x1 = clamp_i32(x, 0, width - 2);
  CVI_S32 y1 = clamp_i32(y, 0, height - 1);
  CVI_S32 x2 = clamp_i32(x + rect_w - 1, 0, width - 1);
  CVI_S32 y2 = clamp_i32(y + rect_h - 1, 0, height - 1);

  x1 = (x1 / 2) * 2;
  x2 = (x2 / 2) * 2;
  if (x2 < x1 || y2 < y1) {
    return;
  }

  for (CVI_S32 row = y1; row <= y2; ++row) {
    for (CVI_S32 col = x1; col <= x2; col += 2) {
      CVI_U8 *p = plane + row * stride + col;
      if (fmt == PIXEL_FORMAT_NV12) {
        p[0] = u;
        p[1] = v;
      } else {
        p[0] = v;
        p[1] = u;
      }
    }
  }
}

static void draw_rect_yuv420sp(VIDEO_FRAME_INFO_S *frame, const TDLBox *box, color_rgb color,
                               CVI_U32 thickness) {
  CVI_S32 width = (CVI_S32)frame->stVFrame.u32Width;
  CVI_S32 height = (CVI_S32)frame->stVFrame.u32Height;
  CVI_S32 x1 = clamp_i32((CVI_S32)box->x1, 0, width - 1);
  CVI_S32 y1 = clamp_i32((CVI_S32)box->y1, 0, height - 1);
  CVI_S32 x2 = clamp_i32((CVI_S32)box->x2, 0, width - 1);
  CVI_S32 y2 = clamp_i32((CVI_S32)box->y2, 0, height - 1);
  CVI_U32 t = thickness < 1 ? 1 : thickness;
  CVI_U8 y = rgb_to_y(&color);
  CVI_U8 u = rgb_to_u(&color);
  CVI_U8 v = rgb_to_v(&color);

  if (x2 <= x1 || y2 <= y1) {
    return;
  }
  fill_y_rect(frame->stVFrame.pu8VirAddr[0], frame->stVFrame.u32Stride[0], width, height, x1, y1,
              x2 - x1 + 1, t, y);
  fill_y_rect(frame->stVFrame.pu8VirAddr[0], frame->stVFrame.u32Stride[0], width, height, x1,
              y2 - (CVI_S32)t + 1, x2 - x1 + 1, t, y);
  fill_y_rect(frame->stVFrame.pu8VirAddr[0], frame->stVFrame.u32Stride[0], width, height, x1, y1,
              t, y2 - y1 + 1, y);
  fill_y_rect(frame->stVFrame.pu8VirAddr[0], frame->stVFrame.u32Stride[0], width, height,
              x2 - (CVI_S32)t + 1, y1, t, y2 - y1 + 1, y);

  CVI_S32 ux1 = (x1 / 2) * 2;
  CVI_S32 ux2 = (x2 / 2) * 2;
  CVI_S32 uy1 = y1 / 2;
  CVI_S32 uy2 = y2 / 2;
  CVI_U32 uv_t = t / 2;
  if (uv_t < 1) uv_t = 1;
  fill_uv_rect(frame->stVFrame.pu8VirAddr[1], frame->stVFrame.u32Stride[1], width, height / 2, ux1,
               uy1, ux2 - ux1 + 2, uv_t, u, v, frame->stVFrame.enPixelFormat);
  fill_uv_rect(frame->stVFrame.pu8VirAddr[1], frame->stVFrame.u32Stride[1], width, height / 2, ux1,
               uy2 - (CVI_S32)uv_t + 1, ux2 - ux1 + 2, uv_t, u, v,
               frame->stVFrame.enPixelFormat);
  fill_uv_rect(frame->stVFrame.pu8VirAddr[1], frame->stVFrame.u32Stride[1], width, height / 2, ux1,
               uy1, uv_t * 2, uy2 - uy1 + 1, u, v, frame->stVFrame.enPixelFormat);
  fill_uv_rect(frame->stVFrame.pu8VirAddr[1], frame->stVFrame.u32Stride[1], width, height / 2,
               ux2 - (CVI_S32)uv_t * 2 + 2, uy1, uv_t * 2, uy2 - uy1 + 1, u, v,
               frame->stVFrame.enPixelFormat);
}

static void draw_rect_yuv420p(VIDEO_FRAME_INFO_S *frame, const TDLBox *box, color_rgb color,
                              CVI_U32 thickness) {
  CVI_S32 width = (CVI_S32)frame->stVFrame.u32Width;
  CVI_S32 height = (CVI_S32)frame->stVFrame.u32Height;
  CVI_S32 x1 = clamp_i32((CVI_S32)box->x1, 0, width - 1);
  CVI_S32 y1 = clamp_i32((CVI_S32)box->y1, 0, height - 1);
  CVI_S32 x2 = clamp_i32((CVI_S32)box->x2, 0, width - 1);
  CVI_S32 y2 = clamp_i32((CVI_S32)box->y2, 0, height - 1);
  CVI_U32 t = thickness < 1 ? 1 : thickness;
  CVI_U8 colors[3] = {rgb_to_y(&color), rgb_to_u(&color), rgb_to_v(&color)};

  if (x2 <= x1 || y2 <= y1) {
    return;
  }

  for (CVI_U32 p = 0; p < 3; ++p) {
    CVI_S32 px1 = x1;
    CVI_S32 px2 = x2;
    CVI_S32 py1 = y1;
    CVI_S32 py2 = y2;
    CVI_U32 pt = t;
    if (p > 0) {
      px1 /= 2;
      px2 /= 2;
      py1 /= 2;
      py2 /= 2;
      pt = t / 2;
      if (pt < 1) pt = 1;
    }
    CVI_S32 plane_width = (p == 0) ? width : width / 2;
    CVI_S32 plane_height = (p == 0) ? height : height / 2;
    fill_y_rect(frame->stVFrame.pu8VirAddr[p], frame->stVFrame.u32Stride[p], plane_width,
                plane_height, px1, py1, px2 - px1 + 1, pt, colors[p]);
    fill_y_rect(frame->stVFrame.pu8VirAddr[p], frame->stVFrame.u32Stride[p], plane_width,
                plane_height, px1, py2 - (CVI_S32)pt + 1, px2 - px1 + 1, pt, colors[p]);
    fill_y_rect(frame->stVFrame.pu8VirAddr[p], frame->stVFrame.u32Stride[p], plane_width,
                plane_height, px1, py1, pt, py2 - py1 + 1, colors[p]);
    fill_y_rect(frame->stVFrame.pu8VirAddr[p], frame->stVFrame.u32Stride[p], plane_width,
                plane_height, px2 - (CVI_S32)pt + 1, py1, pt, py2 - py1 + 1, colors[p]);
  }
}

static CVI_S32 map_frame(VIDEO_FRAME_INFO_S *frame, bool mapped[3]) {
  for (CVI_U32 i = 0; i < 3; ++i) {
    mapped[i] = false;
    if (frame->stVFrame.u64PhyAddr[i] == 0 || frame->stVFrame.u32Length[i] == 0) {
      continue;
    }
    if (frame->stVFrame.pu8VirAddr[i] == NULL) {
      frame->stVFrame.pu8VirAddr[i] =
          (CVI_U8 *)CVI_SYS_Mmap(frame->stVFrame.u64PhyAddr[i], frame->stVFrame.u32Length[i]);
      if (frame->stVFrame.pu8VirAddr[i] == NULL) {
        return CVI_FAILURE;
      }
      mapped[i] = true;
    }
    CVI_SYS_IonFlushCache(frame->stVFrame.u64PhyAddr[i], frame->stVFrame.pu8VirAddr[i],
                          frame->stVFrame.u32Length[i]);
  }
  return CVI_SUCCESS;
}

static void unmap_frame(VIDEO_FRAME_INFO_S *frame, const bool mapped[3]) {
  for (CVI_U32 i = 0; i < 3; ++i) {
    if (mapped[i]) {
      CVI_SYS_Munmap((void *)frame->stVFrame.pu8VirAddr[i], frame->stVFrame.u32Length[i]);
      frame->stVFrame.pu8VirAddr[i] = NULL;
    }
  }
}

TDLBrush SAMPLE_TDL_DefaultBrush(void) {
  TDLBrush brush;
  brush.color.r = DEFAULT_RECT_COLOR_R;
  brush.color.g = DEFAULT_RECT_COLOR_G;
  brush.color.b = DEFAULT_RECT_COLOR_B;
  brush.size = 4;
  return brush;
}

static CVI_S32 draw_boxes(VIDEO_FRAME_INFO_S *frame, const TDLBox *boxes, CVI_U32 size,
                          TDLBrush brush) {
  bool mapped[3] = {false, false, false};
  CVI_S32 ret = map_frame(frame, mapped);
  if (ret != CVI_SUCCESS) {
    return ret;
  }

  if (frame->stVFrame.enPixelFormat == PIXEL_FORMAT_NV12 ||
      frame->stVFrame.enPixelFormat == PIXEL_FORMAT_NV21) {
    for (CVI_U32 i = 0; i < size; ++i) {
      draw_rect_yuv420sp(frame, &boxes[i], brush.color, brush.size);
    }
  } else if (frame->stVFrame.enPixelFormat == PIXEL_FORMAT_YUV_PLANAR_420) {
    for (CVI_U32 i = 0; i < size; ++i) {
      draw_rect_yuv420p(frame, &boxes[i], brush.color, brush.size);
    }
  } else {
    ret = CVI_FAILURE;
  }

  unmap_frame(frame, mapped);
  return ret;
}

CVI_S32 SAMPLE_TDL_DrawObjectRect(const TDLObject *meta, VIDEO_FRAME_INFO_S *frame,
                                  bool draw_text, TDLBrush brush) {
  (void)draw_text;
  if (meta == NULL || frame == NULL || meta->size == 0) {
    return CVI_SUCCESS;
  }

  TDLBox boxes[meta->size];
  for (CVI_U32 i = 0; i < meta->size; ++i) {
    boxes[i] = meta->info[i].box;
  }
  return draw_boxes(frame, boxes, meta->size, brush);
}

CVI_S32 SAMPLE_TDL_DrawFaceRect(const TDLFace *meta, VIDEO_FRAME_INFO_S *frame, bool draw_text,
                                TDLBrush brush) {
  (void)draw_text;
  if (meta == NULL || frame == NULL || meta->size == 0) {
    return CVI_SUCCESS;
  }

  TDLBox boxes[meta->size];
  for (CVI_U32 i = 0; i < meta->size; ++i) {
    boxes[i] = meta->info[i].box;
  }
  return draw_boxes(frame, boxes, meta->size, brush);
}
