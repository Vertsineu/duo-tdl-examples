#pragma once

#include <cvi_comm.h>
#include <cvi_vpss.h>
#include <sample_comm.h>

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace cvi_media {

enum class Codec {
  H264,
  H265,
};

struct Size {
  Size() noexcept = default;
  Size(CVI_U32 init_width, CVI_U32 init_height) noexcept
      : width(init_width), height(init_height) {}

  CVI_U32 width = 0;
  CVI_U32 height = 0;
};

struct VbPoolConfig {
  Size size;
  PIXEL_FORMAT_E pixel_format = VI_PIXEL_FORMAT;
  CVI_U32 block_count = 0;
};

struct VpssChannelConfig {
  VPSS_CHN channel = VPSS_CHN0;
  Size output_size;
  PIXEL_FORMAT_E pixel_format = VI_PIXEL_FORMAT;
  CVI_BOOL keep_aspect_ratio = CVI_TRUE;
  CVI_U32 depth = 1;
  CVI_BOOL attach_vb_pool = CVI_TRUE;
  VbPoolConfig vb_pool;
};

struct PipelineConfig {
  Size stream_size;
  Codec codec = Codec::H264;
  CVI_U32 bitrate_kbps = 8000;
  CVI_S32 source_fps = 30;
  CVI_S32 stream_fps = 30;
  CVI_U32 rtsp_port = 554;
  VPSS_CHN stream_channel = VPSS_CHN0;
  std::vector<VpssChannelConfig> vpss_channels;
  std::vector<VbPoolConfig> extra_vb_pools;
};

class CviError : public std::runtime_error {
 public:
  CviError(const std::string &operation, CVI_S32 code);

  CVI_S32 code() const noexcept { return code_; }

 private:
  CVI_S32 code_;
};

class VideoFrame {
 public:
  VideoFrame() noexcept = default;
  VideoFrame(const VideoFrame &) = delete;
  VideoFrame &operator=(const VideoFrame &) = delete;
  VideoFrame(VideoFrame &&other) noexcept;
  VideoFrame &operator=(VideoFrame &&other) noexcept;
  ~VideoFrame();

  VIDEO_FRAME_INFO_S *get() noexcept { return &frame_; }
  const VIDEO_FRAME_INFO_S *get() const noexcept { return &frame_; }
  bool valid() const noexcept { return acquired_; }
  void reset() noexcept;

 private:
  friend class MediaPipeline;

  VideoFrame(VPSS_GRP group, VPSS_CHN channel, VIDEO_FRAME_INFO_S frame) noexcept;

  VPSS_GRP group_ = 0;
  VPSS_CHN channel_ = 0;
  VIDEO_FRAME_INFO_S frame_{};
  bool acquired_ = false;
};

struct EncodedFrameView {
  PAYLOAD_TYPE_E payload;
  VENC_STREAM_S *stream;
};

class StreamBackend {
 public:
  virtual ~StreamBackend() = default;

  virtual void start(Codec codec, CVI_U32 port) = 0;
  virtual void write(const EncodedFrameView &frame) = 0;
  virtual void stop() noexcept = 0;
  virtual void set_on_client_connected(std::function<void()> callback) = 0;
};

std::unique_ptr<StreamBackend> make_cvi_rtsp_backend();

class MediaPipeline {
 public:
  MediaPipeline();
  explicit MediaPipeline(std::unique_ptr<StreamBackend> backend);
  MediaPipeline(const MediaPipeline &) = delete;
  MediaPipeline &operator=(const MediaPipeline &) = delete;
  ~MediaPipeline();

  void start(const PipelineConfig &config);
  VideoFrame acquire_frame(VPSS_GRP group, VPSS_CHN channel, CVI_S32 timeout_ms);
  void send_frame(VideoFrame &frame);
  void request_idr();
  void stop() noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

PIC_SIZE_E pic_size_from_dimensions(Size size) noexcept;
const char *codec_name(Codec codec) noexcept;
VpssChannelConfig make_vpss_channel(VPSS_CHN channel, Size output_size,
                                    PIXEL_FORMAT_E pixel_format = VI_PIXEL_FORMAT);

}  // namespace cvi_media
