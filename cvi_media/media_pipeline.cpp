#include "media_pipeline.hpp"

#include <cvi_buffer.h>
#include <cvi_isp.h>
#include <cvi_sys.h>
#include <cvi_vb.h>
#include <cvi_venc.h>
#include <cvi_vi.h>
#include <rtsp.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace cvi_media {
namespace {

constexpr VPSS_GRP k_default_vpss_group = 0;
constexpr VPSS_CHN k_default_vpss_channel = VPSS_CHN0;
constexpr VENC_CHN k_default_venc_channel = 0;
constexpr CVI_S32 k_venc_send_frame_timeout_ms = 20000;
constexpr CVI_S32 k_venc_get_stream_timeout_ms = 10000;

std::string make_error_message(const std::string &operation, CVI_S32 code) {
  char buffer[256];
  std::snprintf(buffer, sizeof(buffer), "%s failed with %#x", operation.c_str(), code);
  return buffer;
}

void throw_if_failed(CVI_S32 code, const char *operation) {
  if (code != CVI_SUCCESS) {
    throw CviError(operation, code);
  }
}

void set_default_venc_input(chnInputCfg *cfg, const PipelineConfig &config) {
  std::memset(cfg, 0, sizeof(*cfg));
  std::snprintf(cfg->codec, sizeof(cfg->codec), "%s", codec_name(config.codec));
  cfg->initialDelay = CVI_INITIAL_DELAY_DEFAULT;
  cfg->width = config.stream_size.width;
  cfg->height = config.stream_size.height;
  cfg->vpssGrp = k_default_vpss_group;
  cfg->vpssChn = config.stream_channel;
  cfg->num_frames = -1;
  cfg->bsMode = 0;
  cfg->rcMode = SAMPLE_RC_CBR;
  cfg->iqp = DEF_IQP;
  cfg->pqp = DEF_PQP;
  cfg->gop = DEF_264_GOP;
  cfg->maxIprop = CVI_H26X_MAX_I_PROP_DEFAULT;
  cfg->minIprop = CVI_H26X_MIN_I_PROP_DEFAULT;
  cfg->bitrate = config.bitrate_kbps;
  cfg->firstFrmstartQp = 30;
  cfg->minIqp = DEF_264_MINIQP;
  cfg->maxIqp = DEF_264_MAXIQP;
  cfg->minQp = DEF_264_MINQP;
  cfg->maxQp = DEF_264_MAXQP;
  cfg->srcFramerate = config.source_fps;
  cfg->framerate = config.stream_fps;
  cfg->bVariFpsEn = 0;
  cfg->maxbitrate = -1;
  cfg->statTime = -1;
  cfg->chgNum = -1;
  cfg->quality = -1;
  cfg->pixel_format = 0;
  cfg->bitstreamBufSize = 0;
  cfg->single_LumaBuf = 0;
  cfg->single_core = 0;
  cfg->forceIdr = -1;
  cfg->tempLayer = 0;
  cfg->testRoi = 0;
  cfg->bgInterval = 0;
#ifdef CV186X
  cfg->u32GopPreset = GOP_PRESET_IDX_IPPPP;
#endif
}

void set_default_sensor_ini(SAMPLE_INI_CFG_S *cfg) {
  std::memset(cfg, 0, sizeof(*cfg));
  cfg->enSource = VI_PIPE_FRAME_SOURCE_DEV;
  cfg->devNum = 1;
  cfg->enSnsType[0] = SONY_IMX327_MIPI_2M_30FPS_12BIT;
  cfg->enWDRMode[0] = WDR_MODE_NONE;
  cfg->s32BusId[0] = 3;
  cfg->s32SnsI2cAddr[0] = -1;
  cfg->MipiDev[0] = 0xFF;
  cfg->u8UseMultiSns = 0;
}

SAMPLE_VI_CONFIG_S load_vi_config() {
  SAMPLE_INI_CFG_S ini_config;
  set_default_sensor_ini(&ini_config);

  if (SAMPLE_COMM_VI_ParseIni(&ini_config)) {
    std::printf("sensor info is loaded from ini file.\n");
  }

  SAMPLE_VI_CONFIG_S vi_config;
  std::memset(&vi_config, 0, sizeof(vi_config));
  throw_if_failed(SAMPLE_COMM_VI_IniToViCfg(&ini_config, &vi_config),
                  "SAMPLE_COMM_VI_IniToViCfg");
  if (vi_config.s32WorkingViNum <= 0) {
    throw CviError("invalid VI working device count", CVI_FAILURE);
  }
  return vi_config;
}

Size sensor_size_from_vi_config(const SAMPLE_VI_CONFIG_S &vi_config) {
  PIC_SIZE_E pic_size = PIC_BUTT;
  throw_if_failed(
      SAMPLE_COMM_VI_GetSizeBySensor(vi_config.astViInfo[0].stSnsInfo.enSnsType, &pic_size),
      "SAMPLE_COMM_VI_GetSizeBySensor");

  SIZE_S sdk_size;
  throw_if_failed(SAMPLE_COMM_SYS_GetPicSize(pic_size, &sdk_size), "SAMPLE_COMM_SYS_GetPicSize");
  return Size{sdk_size.u32Width, sdk_size.u32Height};
}

std::vector<VpssChannelConfig> effective_channels(const PipelineConfig &config) {
  if (!config.vpss_channels.empty()) {
    return config.vpss_channels;
  }

  VpssChannelConfig stream_channel;
  stream_channel.channel = config.stream_channel;
  stream_channel.output_size = config.stream_size;
  stream_channel.pixel_format = VI_PIXEL_FORMAT;
  stream_channel.keep_aspect_ratio = CVI_TRUE;
  stream_channel.depth = 1;
  stream_channel.attach_vb_pool = CVI_TRUE;
  stream_channel.vb_pool.size = config.stream_size;
  stream_channel.vb_pool.pixel_format = VI_PIXEL_FORMAT;
#ifdef CV180X
  stream_channel.vb_pool.block_count = 2;
#else
  stream_channel.vb_pool.block_count = 5;
#endif
  return std::vector<VpssChannelConfig>(1, stream_channel);
}

CVI_U32 default_block_count() {
#ifdef CV180X
  return 2;
#else
  return 5;
#endif
}

VbPoolConfig normalized_pool_config(const VpssChannelConfig &channel) {
  VbPoolConfig pool = channel.vb_pool;
  if (pool.size.width == 0 || pool.size.height == 0) {
    pool.size = channel.output_size;
  }
  if (pool.block_count == 0) {
    pool.block_count = default_block_count();
  }
  return pool;
}

VB_CONFIG_S make_vb_config(const std::vector<VpssChannelConfig> &channels,
                           const std::vector<VbPoolConfig> &extra_pools) {
  VB_CONFIG_S vb_config;
  std::memset(&vb_config, 0, sizeof(vb_config));
  std::vector<VbPoolConfig> pools;
  for (std::size_t i = 0; i < channels.size(); ++i) {
    if (channels[i].attach_vb_pool) {
      pools.push_back(normalized_pool_config(channels[i]));
    }
  }
  pools.insert(pools.end(), extra_pools.begin(), extra_pools.end());

  if (pools.empty() || pools.size() >= VB_MAX_COMM_POOLS) {
    throw CviError("invalid VB pool count", CVI_FAILURE);
  }

  vb_config.u32MaxPoolCnt = static_cast<CVI_U32>(pools.size());
  for (std::size_t i = 0; i < pools.size(); ++i) {
    VbPoolConfig pool = pools[i];
    if (pool.block_count == 0) {
      pool.block_count = default_block_count();
    }
    CVI_U32 block_size = COMMON_GetPicBufferSize(pool.size.width, pool.size.height,
                                                 pool.pixel_format, DATA_BITWIDTH_8,
                                                 COMPRESS_MODE_NONE, DEFAULT_ALIGN);
    vb_config.astCommPool[i].u32BlkSize = block_size;
    vb_config.astCommPool[i].u32BlkCnt = pool.block_count;
    std::printf("Create VBPool[%zu], size: (%u * %u) = %u bytes\n", i, block_size,
                pool.block_count, block_size * pool.block_count);
  }

  return vb_config;
}

VPSS_GRP_ATTR_S make_vpss_group_attr(Size sensor_size) {
  VPSS_GRP_ATTR_S attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.stFrameRate.s32SrcFrameRate = -1;
  attr.stFrameRate.s32DstFrameRate = -1;
  attr.enPixelFormat = VI_PIXEL_FORMAT;
  attr.u32MaxW = sensor_size.width;
  attr.u32MaxH = sensor_size.height;
#ifndef CV186X
  attr.u8VpssDev = 1;
#endif
  return attr;
}

VPSS_CHN_ATTR_S make_vpss_channel_attr(const VpssChannelConfig &channel) {
  VPSS_CHN_ATTR_S attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.u32Width = channel.output_size.width;
  attr.u32Height = channel.output_size.height;
  attr.enVideoFormat = VIDEO_FORMAT_LINEAR;
  attr.enPixelFormat = channel.pixel_format;
  attr.stFrameRate.s32SrcFrameRate = -1;
  attr.stFrameRate.s32DstFrameRate = -1;
  attr.u32Depth = channel.depth;
  attr.bMirror = CVI_FALSE;
  attr.bFlip = CVI_FALSE;
  if (channel.keep_aspect_ratio) {
    attr.stAspectRatio.enMode = ASPECT_RATIO_AUTO;
    attr.stAspectRatio.u32BgColor = RGB_8BIT(0, 0, 0);
  } else {
    attr.stAspectRatio.enMode = ASPECT_RATIO_NONE;
  }
  attr.stNormalize.bEnable = CVI_FALSE;
  attr.stNormalize.rounding = VPSS_ROUNDING_TO_EVEN;
  return attr;
}

PAYLOAD_TYPE_E payload_from_codec(Codec codec) {
  switch (codec) {
    case Codec::H264:
      return PT_H264;
    case Codec::H265:
      return PT_H265;
  }
  return PT_BUTT;
}

CVI_RTSP_VIDEO_CODEC rtsp_codec_from_codec(Codec codec) {
  switch (codec) {
    case Codec::H264:
      return RTSP_VIDEO_H264;
    case Codec::H265:
      return RTSP_VIDEO_H265;
  }
  return RTSP_VIDEO_NONE;
}

bool pack_has_idr(const VENC_PACK_S &pack, PAYLOAD_TYPE_E payload) {
  if (pack.u32Offset >= pack.u32Len) {
    return false;
  }

  CVI_U8 *data = pack.pu8Addr + pack.u32Offset;
  CVI_U32 len = pack.u32Len - pack.u32Offset;

  if (payload == PT_H264) {
    if (pack.DataType.enH264EType == H264E_NALU_IDRSLICE) {
      return true;
    }
    for (CVI_U32 i = 0; i < pack.u32DataNum && i < 8; ++i) {
      if (pack.stPackInfo[i].u32PackType.enH264EType == H264E_NALU_IDRSLICE) {
        return true;
      }
    }
    for (CVI_U32 i = 0; i + 4 < len; ++i) {
      CVI_U32 start_code_len = 0;
      if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
        start_code_len = 3;
      } else if (i + 5 < len && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 &&
                 data[i + 3] == 1) {
        start_code_len = 4;
      }
      if (start_code_len > 0 && i + start_code_len < len &&
          (data[i + start_code_len] & 0x1f) == H264E_NALU_IDRSLICE) {
        return true;
      }
    }
  } else if (payload == PT_H265) {
    if (pack.DataType.enH265EType == H265E_NALU_IDRSLICE) {
      return true;
    }
    for (CVI_U32 i = 0; i < pack.u32DataNum && i < 8; ++i) {
      if (pack.stPackInfo[i].u32PackType.enH265EType == H265E_NALU_IDRSLICE) {
        return true;
      }
    }
    for (CVI_U32 i = 0; i + 5 < len; ++i) {
      CVI_U32 start_code_len = 0;
      if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
        start_code_len = 3;
      } else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 &&
                 data[i + 3] == 1) {
        start_code_len = 4;
      }
      if (start_code_len > 0 && i + start_code_len + 1 < len) {
        CVI_U8 nal_type = (data[i + start_code_len] >> 1) & 0x3f;
        if (nal_type == H265E_NALU_IDRSLICE || nal_type == 20) {
          return true;
        }
      }
    }
  }

  return false;
}

bool stream_has_idr(const VENC_STREAM_S &stream, PAYLOAD_TYPE_E payload) {
  for (CVI_U32 i = 0; i < stream.u32PackCount; ++i) {
    if (pack_has_idr(stream.pstPack[i], payload)) {
      return true;
    }
  }
  return false;
}

void request_venc_idr(VENC_CHN channel) {
  CVI_S32 ret = CVI_VENC_RequestIDR(channel, CVI_TRUE);
  if (ret != CVI_SUCCESS) {
    std::printf("CVI_VENC_RequestIDR failed with %#x\n", ret);
  }
}

class CviRtspBackend final : public StreamBackend {
 public:
  ~CviRtspBackend() override { stop(); }

  void start(Codec codec, CVI_U32 port) override {
    CVI_RTSP_CONFIG config;
    std::memset(&config, 0, sizeof(config));
    config.port = static_cast<int>(port);

    if (CVI_RTSP_Create(&context_, &config) < 0) {
      throw CviError("CVI_RTSP_Create", CVI_FAILURE);
    }

    CVI_RTSP_SESSION_ATTR attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.video.codec = rtsp_codec_from_codec(codec);
    std::snprintf(attr.name, sizeof(attr.name), "%s", codec_name(codec));

    if (CVI_RTSP_CreateSession(context_, &attr, &session_) < 0) {
      throw CviError("CVI_RTSP_CreateSession", CVI_FAILURE);
    }

    CVI_RTSP_STATE_LISTENER listener;
    std::memset(&listener, 0, sizeof(listener));
    listener.onConnect = &CviRtspBackend::on_connect;
    listener.argConn = this;
    listener.onDisconnect = &CviRtspBackend::on_disconnect;
    listener.argDisconn = this;
    if (CVI_RTSP_SetListener(context_, &listener) < 0) {
      throw CviError("CVI_RTSP_SetListener", CVI_FAILURE);
    }

    if (CVI_RTSP_Start(context_) < 0) {
      throw CviError("CVI_RTSP_Start", CVI_FAILURE);
    }

    started_ = true;
  }

  void write(const EncodedFrameView &frame) override {
    if (frame.stream->u32PackCount > CVI_RTSP_DATA_MAX_BLOCK) {
      throw CviError("too many VENC packs for RTSP frame", CVI_FAILURE);
    }

    CVI_RTSP_DATA data;
    std::memset(&data, 0, sizeof(data));
    data.blockCnt = frame.stream->u32PackCount;
    for (CVI_U32 i = 0; i < frame.stream->u32PackCount; ++i) {
      VENC_PACK_S &pack = frame.stream->pstPack[i];
      data.dataPtr[i] = pack.pu8Addr + pack.u32Offset;
      data.dataLen[i] = pack.u32Len - pack.u32Offset;
    }

    if (CVI_RTSP_WriteFrame(context_, session_->video, &data) != CVI_SUCCESS) {
      throw CviError("CVI_RTSP_WriteFrame", CVI_FAILURE);
    }
  }

  void stop() noexcept override {
    if (context_ != nullptr && started_) {
      CVI_RTSP_Stop(context_);
      started_ = false;
    }
    if (context_ != nullptr && session_ != nullptr) {
      CVI_RTSP_DestroySession(context_, session_);
      session_ = nullptr;
    }
    if (context_ != nullptr) {
      CVI_RTSP_Destroy(&context_);
      context_ = nullptr;
    }
  }

  void set_on_client_connected(std::function<void()> callback) override {
    on_client_connected_ = std::move(callback);
  }

 private:
  static void on_connect(const char *ip, void *arg) {
    std::printf("RTSP client connected from: %s\n", ip);
    auto *self = static_cast<CviRtspBackend *>(arg);
    if (self != nullptr && self->on_client_connected_) {
      self->on_client_connected_();
    }
  }

  static void on_disconnect(const char *ip, void *) {
    std::printf("RTSP client disconnected from: %s\n", ip);
  }

  CVI_RTSP_CTX *context_ = nullptr;
  CVI_RTSP_SESSION *session_ = nullptr;
  bool started_ = false;
  std::function<void()> on_client_connected_;
};

class VencEncoder {
 public:
  void start(const PipelineConfig &config) {
    config_ = config;
    set_default_venc_input(&input_config_, config);

    VENC_GOP_ATTR_S gop_attr;
    throw_if_failed(SAMPLE_COMM_VENC_GetGopAttr(VENC_GOPMODE_NORMALP, &gop_attr),
                    "SAMPLE_COMM_VENC_GetGopAttr");

    PAYLOAD_TYPE_E payload = payload_from_codec(config.codec);
    PIC_SIZE_E pic_size = pic_size_from_dimensions(config.stream_size);
    if (payload == PT_BUTT || pic_size == PIC_BUTT) {
      throw CviError("unsupported VENC codec or picture size", CVI_FAILURE);
    }

    std::printf("Initialize VENC\n");
    std::printf("venc codec: %s\n", input_config_.codec);
    std::printf("venc frame size: %ux%u\n", config.stream_size.width, config.stream_size.height);

    throw_if_failed(SAMPLE_COMM_VENC_Start(&input_config_, k_default_venc_channel, payload, pic_size,
                                           static_cast<SAMPLE_RC_E>(input_config_.rcMode), 0,
                                           CVI_FALSE, &gop_attr),
                    "SAMPLE_COMM_VENC_Start");
    started_ = true;
    need_idr_ = true;
    request_idr();
  }

  void send(VideoFrame &frame, StreamBackend &backend) {
    throw_if_failed(CVI_VENC_SendFrame(k_default_venc_channel, frame.get(), k_venc_send_frame_timeout_ms),
                    "CVI_VENC_SendFrame");

    VENC_CHN_ATTR_S channel_attr;
    throw_if_failed(CVI_VENC_GetChnAttr(k_default_venc_channel, &channel_attr),
                    "CVI_VENC_GetChnAttr");

    VENC_CHN_STATUS_S status;
    throw_if_failed(CVI_VENC_QueryStatus(k_default_venc_channel, &status),
                    "CVI_VENC_QueryStatus");
    if (status.u32CurPacks == 0) {
      std::printf("NOTE: Current frame is NULL!\n");
      return;
    }

    std::vector<VENC_PACK_S> packs(status.u32CurPacks);
    VENC_STREAM_S stream;
    std::memset(&stream, 0, sizeof(stream));
    stream.pstPack = packs.data();

    throw_if_failed(CVI_VENC_GetStream(k_default_venc_channel, &stream, k_venc_get_stream_timeout_ms),
                    "CVI_VENC_GetStream");

    struct StreamGuard {
      ~StreamGuard() {
        if (stream != nullptr) {
          CVI_VENC_ReleaseStream(channel, stream);
        }
      }
      VENC_CHN channel;
      VENC_STREAM_S *stream;
    } guard{k_default_venc_channel, &stream};

    PAYLOAD_TYPE_E payload = channel_attr.stVencAttr.enType;
    if (need_idr_) {
      if (!stream_has_idr(stream, payload)) {
        return;
      }
      need_idr_ = false;
    }

    EncodedFrameView view{payload, &stream};
    backend.write(view);
  }

  void request_idr() {
    need_idr_ = true;
    if (started_) {
      request_venc_idr(k_default_venc_channel);
    }
  }

  void stop() noexcept {
    if (started_) {
      SAMPLE_COMM_VENC_Stop(k_default_venc_channel);
      started_ = false;
    }
  }

 private:
  PipelineConfig config_{};
  chnInputCfg input_config_{};
  bool started_ = false;
  bool need_idr_ = false;
};

}  // namespace

class MediaPipeline::Impl {
 public:
  explicit Impl(std::unique_ptr<StreamBackend> backend) : backend_(std::move(backend)) {}

  void start(const PipelineConfig &config) {
    config_ = config;

    MMF_VERSION_S version;
    if (CVI_SYS_GetVersion(&version) == CVI_SUCCESS) {
      std::printf("MMF Version:%s\n", version.version);
    }

    vi_config_ = load_vi_config();
    Size sensor_size = sensor_size_from_vi_config(vi_config_);
    channels_ = effective_channels(config);

    std::printf("Initialize SYS and VB\n");
    CVI_SYS_Exit();
    CVI_VB_Exit();
    VB_CONFIG_S vb_config = make_vb_config(channels_, config.extra_vb_pools);
    throw_if_failed(CVI_VB_SetConfig(&vb_config), "CVI_VB_SetConfig");
    throw_if_failed(CVI_VB_Init(), "CVI_VB_Init");
    vb_initialized_ = true;
    throw_if_failed(CVI_SYS_Init(), "CVI_SYS_Init");
    sys_initialized_ = true;

    std::printf("Initialize VI\n");
    throw_if_failed(CVI_VI_SetDevNum(vi_config_.s32WorkingViNum), "CVI_VI_SetDevNum");
    VI_VPSS_MODE_S vi_vpss_mode;
    std::memset(&vi_vpss_mode, 0, sizeof(vi_vpss_mode));
    vi_vpss_mode.aenMode[0] = VI_OFFLINE_VPSS_ONLINE;
    throw_if_failed(CVI_SYS_SetVIVPSSMode(&vi_vpss_mode), "CVI_SYS_SetVIVPSSMode");
    throw_if_failed(SAMPLE_PLAT_VI_INIT(&vi_config_), "SAMPLE_PLAT_VI_INIT");
    vi_started_ = true;

    ISP_PUB_ATTR_S pub_attr;
    std::memset(&pub_attr, 0, sizeof(pub_attr));
    if (CVI_ISP_GetPubAttr(0, &pub_attr) == CVI_SUCCESS) {
      pub_attr.f32FrameRate = config.source_fps;
      CVI_ISP_SetPubAttr(0, &pub_attr);
    }

    std::printf("Initialize VPSS\n");
#ifndef CV186X
    VPSS_MODE_S vpss_mode;
    std::memset(&vpss_mode, 0, sizeof(vpss_mode));
    vpss_mode.aenInput[0] = VPSS_INPUT_MEM;
    vpss_mode.enMode = VPSS_MODE_DUAL;
    vpss_mode.ViPipe[0] = 0;
    vpss_mode.aenInput[1] = VPSS_INPUT_ISP;
    vpss_mode.ViPipe[1] = 0;
    throw_if_failed(CVI_SYS_SetVPSSModeEx(&vpss_mode), "CVI_SYS_SetVPSSModeEx");
#endif

    VPSS_GRP_ATTR_S group_attr = make_vpss_group_attr(sensor_size);
    throw_if_failed(CVI_VPSS_CreateGrp(k_default_vpss_group, &group_attr), "CVI_VPSS_CreateGrp");
    vpss_group_created_ = true;
    throw_if_failed(CVI_VPSS_ResetGrp(k_default_vpss_group), "CVI_VPSS_ResetGrp");
    for (std::size_t i = 0; i < channels_.size(); ++i) {
      VPSS_CHN_ATTR_S channel_attr = make_vpss_channel_attr(channels_[i]);
      throw_if_failed(CVI_VPSS_SetChnAttr(k_default_vpss_group, channels_[i].channel, &channel_attr),
                      "CVI_VPSS_SetChnAttr");
      throw_if_failed(CVI_VPSS_EnableChn(k_default_vpss_group, channels_[i].channel),
                      "CVI_VPSS_EnableChn");
      enabled_channels_.push_back(channels_[i].channel);
    }
    throw_if_failed(CVI_VPSS_StartGrp(k_default_vpss_group), "CVI_VPSS_StartGrp");
    vpss_started_ = true;

    MMF_CHN_S vi_channel = {CVI_ID_VI, 0, 0};
    MMF_CHN_S vpss_channel = {CVI_ID_VPSS, k_default_vpss_group, 0};
    throw_if_failed(CVI_SYS_Bind(&vi_channel, &vpss_channel), "CVI_SYS_Bind(VI->VPSS)");
    vi_bound_to_vpss_ = true;

    CVI_U32 attach_pool_index = 0;
    for (std::size_t i = 0; i < channels_.size(); ++i) {
      if (!channels_[i].attach_vb_pool) {
        continue;
      }
      throw_if_failed(CVI_VPSS_AttachVbPool(k_default_vpss_group, channels_[i].channel,
                                            static_cast<VB_POOL>(attach_pool_index)),
                      "CVI_VPSS_AttachVbPool");
      attached_channels_.push_back(channels_[i].channel);
      ++attach_pool_index;
    }

    encoder_.start(config);
    backend_->set_on_client_connected([this]() { request_idr(); });
    std::printf("Initialize stream backend\n");
    backend_->start(config.codec, config.rtsp_port);
    started_ = true;
  }

  VideoFrame acquire_frame(VPSS_GRP group, VPSS_CHN channel, CVI_S32 timeout_ms) {
    VIDEO_FRAME_INFO_S frame;
    std::memset(&frame, 0, sizeof(frame));
    throw_if_failed(CVI_VPSS_GetChnFrame(group, channel, &frame, timeout_ms),
                    "CVI_VPSS_GetChnFrame");
    return VideoFrame(group, channel, frame);
  }

  void send_frame(VideoFrame &frame) { encoder_.send(frame, *backend_); }

  void request_idr() { encoder_.request_idr(); }

  void stop() noexcept {
    backend_->stop();
    encoder_.stop();

    for (std::size_t i = 0; i < attached_channels_.size(); ++i) {
      CVI_VPSS_DetachVbPool(k_default_vpss_group, attached_channels_[i]);
    }
    attached_channels_.clear();

    if (vi_bound_to_vpss_) {
      MMF_CHN_S vi_channel = {CVI_ID_VI, 0, 0};
      MMF_CHN_S vpss_channel = {CVI_ID_VPSS, k_default_vpss_group, 0};
      CVI_SYS_UnBind(&vi_channel, &vpss_channel);
      vi_bound_to_vpss_ = false;
    }

    if (vpss_started_) {
      CVI_VPSS_StopGrp(k_default_vpss_group);
      vpss_started_ = false;
    }
    for (std::size_t i = 0; i < enabled_channels_.size(); ++i) {
      CVI_VPSS_DisableChn(k_default_vpss_group, enabled_channels_[i]);
    }
    enabled_channels_.clear();
    if (vpss_group_created_) {
      CVI_VPSS_DestroyGrp(k_default_vpss_group);
      vpss_group_created_ = false;
    }

    if (vi_started_) {
      SAMPLE_COMM_VI_DestroyIsp(&vi_config_);
      SAMPLE_COMM_VI_DestroyVi(&vi_config_);
      vi_started_ = false;
    }

    if (sys_initialized_) {
      CVI_SYS_Exit();
      sys_initialized_ = false;
    }
    if (vb_initialized_) {
      CVI_VB_Exit();
      vb_initialized_ = false;
    }

    started_ = false;
  }

 private:
  PipelineConfig config_{};
  SAMPLE_VI_CONFIG_S vi_config_{};
  std::unique_ptr<StreamBackend> backend_;
  VencEncoder encoder_;
  std::vector<VpssChannelConfig> channels_;
  std::vector<VPSS_CHN> enabled_channels_;
  std::vector<VPSS_CHN> attached_channels_;
  bool started_ = false;
  bool sys_initialized_ = false;
  bool vb_initialized_ = false;
  bool vi_started_ = false;
  bool vpss_group_created_ = false;
  bool vpss_started_ = false;
  bool vi_bound_to_vpss_ = false;
};

CviError::CviError(const std::string &operation, CVI_S32 code)
    : std::runtime_error(make_error_message(operation, code)), code_(code) {}

VideoFrame::VideoFrame(VPSS_GRP group, VPSS_CHN channel, VIDEO_FRAME_INFO_S frame) noexcept
    : group_(group), channel_(channel), frame_(frame), acquired_(true) {}

VideoFrame::VideoFrame(VideoFrame &&other) noexcept { *this = std::move(other); }

VideoFrame &VideoFrame::operator=(VideoFrame &&other) noexcept {
  if (this != &other) {
    reset();
    group_ = other.group_;
    channel_ = other.channel_;
    frame_ = other.frame_;
    acquired_ = other.acquired_;
    other.acquired_ = false;
  }
  return *this;
}

VideoFrame::~VideoFrame() { reset(); }

void VideoFrame::reset() noexcept {
  if (acquired_) {
    CVI_VPSS_ReleaseChnFrame(group_, channel_, &frame_);
    acquired_ = false;
  }
}

std::unique_ptr<StreamBackend> make_cvi_rtsp_backend() {
  return std::unique_ptr<StreamBackend>(new CviRtspBackend());
}

MediaPipeline::MediaPipeline() : MediaPipeline(make_cvi_rtsp_backend()) {}

MediaPipeline::MediaPipeline(std::unique_ptr<StreamBackend> backend)
    : impl_(new Impl(std::move(backend))) {}

MediaPipeline::~MediaPipeline() { stop(); }

void MediaPipeline::start(const PipelineConfig &config) { impl_->start(config); }

VideoFrame MediaPipeline::acquire_frame(VPSS_GRP group, VPSS_CHN channel, CVI_S32 timeout_ms) {
  return impl_->acquire_frame(group, channel, timeout_ms);
}

void MediaPipeline::send_frame(VideoFrame &frame) { impl_->send_frame(frame); }

void MediaPipeline::request_idr() { impl_->request_idr(); }

void MediaPipeline::stop() noexcept {
  if (impl_) {
    impl_->stop();
  }
}

PIC_SIZE_E pic_size_from_dimensions(Size size) noexcept {
  if (size.width == 1280 && size.height == 720) {
    return PIC_720P;
  }
  if (size.width == 1920 && size.height == 1080) {
    return PIC_1080P;
  }
  if (size.width == 3840 && size.height == 2160) {
    return PIC_3840x2160;
  }
  if (size.width == 2560 && size.height == 1440) {
    return PIC_1440P;
  }
  return PIC_BUTT;
}

const char *codec_name(Codec codec) noexcept {
  switch (codec) {
    case Codec::H264:
      return "h264";
    case Codec::H265:
      return "h265";
  }
  return "unknown";
}

VpssChannelConfig make_vpss_channel(VPSS_CHN channel, Size output_size,
                                    PIXEL_FORMAT_E pixel_format) {
  VpssChannelConfig config;
  config.channel = channel;
  config.output_size = output_size;
  config.pixel_format = pixel_format;
  config.keep_aspect_ratio = CVI_TRUE;
  config.depth = 1;
  config.attach_vb_pool = CVI_TRUE;
  config.vb_pool.size = output_size;
  config.vb_pool.pixel_format = pixel_format;
  config.vb_pool.block_count = 0;
  return config;
}

}  // namespace cvi_media
