#define LOG_TAG "SampleVIRtsp"
#define LOG_LEVEL LOG_LEVEL_INFO

#include "middleware_utils.h"

#include <core/utils/vpss_helper.h>
#include <cvi_comm.h>
#include <sample_comm.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <system_error>
#include <thread>

namespace {

constexpr CVI_U32 kDefaultWidth = 1280;
constexpr CVI_U32 kDefaultHeight = 720;
constexpr const char *kDefaultCodec = "h264";
constexpr CVI_S32 kFrameTimeoutMs = 2000;

volatile std::sig_atomic_t gSignalExitRequested = 0;
std::atomic<bool> gThreadExitRequested(false);

struct ProgramOptions {
  ProgramOptions() : vencSize(), codec(kDefaultCodec) {
    vencSize.u32Width = kDefaultWidth;
    vencSize.u32Height = kDefaultHeight;
  }

  SIZE_S vencSize;
  std::string codec;
};

class Middleware {
 public:
  Middleware() = default;
  Middleware(const Middleware &) = delete;
  Middleware &operator=(const Middleware &) = delete;

  ~Middleware() {
    if (initialized_) {
      SAMPLE_TDL_Destroy_MW(&context_);
    }
  }

  CVI_S32 init(SAMPLE_TDL_MW_CONFIG_S *config) {
    const CVI_S32 ret = SAMPLE_TDL_Init_WM(config, &context_);
    initialized_ = (ret == CVI_SUCCESS);
    return ret;
  }

  SAMPLE_TDL_MW_CONTEXT &get() { return context_; }

 private:
  SAMPLE_TDL_MW_CONTEXT context_{};
  bool initialized_ = false;
};

class VpssFrame {
 public:
  VpssFrame(VPSS_GRP group, VPSS_CHN channel)
      : group_(group), channel_(channel) {}
  VpssFrame(const VpssFrame &) = delete;
  VpssFrame &operator=(const VpssFrame &) = delete;

  ~VpssFrame() { release(); }

  CVI_S32 acquire(CVI_S32 timeoutMs) {
    const CVI_S32 ret =
        CVI_VPSS_GetChnFrame(group_, channel_, &frame_, timeoutMs);
    acquired_ = (ret == CVI_SUCCESS);
    return ret;
  }

  VIDEO_FRAME_INFO_S *get() { return &frame_; }

 private:
  void release() {
    if (acquired_) {
      CVI_VPSS_ReleaseChnFrame(group_, channel_, &frame_);
      acquired_ = false;
    }
  }

  VPSS_GRP group_;
  VPSS_CHN channel_;
  VIDEO_FRAME_INFO_S frame_{};
  bool acquired_ = false;
};

static bool exit_requested() {
  return gSignalExitRequested != 0 ||
         gThreadExitRequested.load(std::memory_order_relaxed);
}

static void request_exit() {
  gThreadExitRequested.store(true, std::memory_order_relaxed);
}

static void SampleHandleSig(int signo) {
  if (SIGINT == signo || SIGTERM == signo) {
    gSignalExitRequested = 1;
  }
}

static bool parse_u32(const char *text, CVI_U32 &value) {
  if (text == nullptr || *text == '\0' || *text == '-') {
    return false;
  }

  errno = 0;
  char *end = nullptr;
  const unsigned long parsed = std::strtoul(text, &end, 10);
  if (errno == ERANGE || end == text || *end != '\0' || parsed == 0 ||
      parsed > std::numeric_limits<CVI_U32>::max()) {
    return false;
  }

  value = static_cast<CVI_U32>(parsed);
  return true;
}

static bool is_supported_codec(const std::string &codec) {
  return codec == "h264" || codec == "h265";
}

static void print_usage(const char *program) {
  std::printf("\nUsage: %s [WIDTH HEIGHT [h264|h265]]\n\n", program);
  std::printf("Default: %s %u %u %s\n", program, kDefaultWidth, kDefaultHeight,
              kDefaultCodec);
  std::printf("RTSP URL: rtsp://<duo-ip>/%s\n\n", kDefaultCodec);
}

static CVI_S32 parse_options(int argc, char *argv[], ProgramOptions &options) {
  if (argc != 1 && argc != 3 && argc != 4) {
    print_usage(argv[0]);
    return CVI_FAILURE;
  }

  if (argc >= 3 && (!parse_u32(argv[1], options.vencSize.u32Width) ||
                    !parse_u32(argv[2], options.vencSize.u32Height))) {
    std::printf("Invalid width or height.\n");
    return CVI_FAILURE;
  }

  if (argc == 4) {
    options.codec = argv[3];
    if (!is_supported_codec(options.codec)) {
      std::printf("Unsupported codec: %s. Use h264 or h265.\n", argv[3]);
      return CVI_FAILURE;
    }
  }

  if (SAMPLE_TDL_Get_PIC_Size(options.vencSize.u32Width,
                              options.vencSize.u32Height) == PIC_BUTT) {
    std::printf("Unsupported frame size: %ux%u. Try 1280x720 or 1920x1080.\n",
                options.vencSize.u32Width, options.vencSize.u32Height);
    return CVI_FAILURE;
  }

  return CVI_SUCCESS;
}

static CVI_S32 run_rtsp(SAMPLE_TDL_MW_CONTEXT &mwContext) {
  std::printf("Enter RTSP thread\n");

  CVI_S32 result = CVI_SUCCESS;
  while (!exit_requested()) {
    VpssFrame frame(static_cast<VPSS_GRP>(0), VPSS_CHN0);
    CVI_S32 s32Ret = frame.acquire(kFrameTimeoutMs);
    if (s32Ret != CVI_SUCCESS) {
      if (exit_requested()) {
        break;
      }
      std::printf("CVI_VPSS_GetChnFrame chn0 failed with %#x\n", s32Ret);
      request_exit();
      result = s32Ret;
      break;
    }

    if (exit_requested()) {
      break;
    }

    s32Ret = SAMPLE_TDL_Send_Frame_RTSP(frame.get(), &mwContext);
    if (s32Ret != CVI_SUCCESS) {
      std::printf("Send RTSP frame failed, ret=%#x\n", s32Ret);
      request_exit();
      result = s32Ret;
      break;
    }
  }

  std::printf("Exit RTSP thread\n");
  return result;
}

static CVI_S32 get_middleware_config(SAMPLE_TDL_MW_CONFIG_S *pstMWConfig,
                                     const SIZE_S &stVencSize,
                                     const std::string &codec) {
  CVI_S32 s32Ret = SAMPLE_TDL_Get_VI_Config(&pstMWConfig->stViConfig);
  if (s32Ret != CVI_SUCCESS || pstMWConfig->stViConfig.s32WorkingViNum <= 0) {
    std::printf("Failed to get sensor information from ini file "
                "(/mnt/data/sensor_cfg.ini).\n");
    return CVI_FAILURE;
  }

  PIC_SIZE_E enPicSize;
  s32Ret = SAMPLE_COMM_VI_GetSizeBySensor(
      pstMWConfig->stViConfig.astViInfo[0].stSnsInfo.enSnsType, &enPicSize);
  if (s32Ret != CVI_SUCCESS) {
    std::printf("Cannot get sensor size\n");
    return s32Ret;
  }

  SIZE_S stSensorSize;
  s32Ret = SAMPLE_COMM_SYS_GetPicSize(enPicSize, &stSensorSize);
  if (s32Ret != CVI_SUCCESS) {
    std::printf("Cannot get sensor size\n");
    return s32Ret;
  }

  pstMWConfig->stVBPoolConfig.u32VBPoolCount = 1;
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[0].enFormat = VI_PIXEL_FORMAT;
#ifdef CV180X
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[0].u32BlkCount = 2;
#else
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[0].u32BlkCount = 5;
#endif
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[0].u32Height =
      stVencSize.u32Height;
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[0].u32Width = stVencSize.u32Width;
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[0].bBind = true;
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[0].u32VpssChnBinding = VPSS_CHN0;
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[0].u32VpssGrpBinding = (VPSS_GRP)0;

  pstMWConfig->stVPSSPoolConfig.u32VpssGrpCount = 1;
#ifndef CV186X
  pstMWConfig->stVPSSPoolConfig.stVpssMode.aenInput[0] = VPSS_INPUT_MEM;
  pstMWConfig->stVPSSPoolConfig.stVpssMode.enMode = VPSS_MODE_DUAL;
  pstMWConfig->stVPSSPoolConfig.stVpssMode.ViPipe[0] = 0;
  pstMWConfig->stVPSSPoolConfig.stVpssMode.aenInput[1] = VPSS_INPUT_ISP;
  pstMWConfig->stVPSSPoolConfig.stVpssMode.ViPipe[1] = 0;
#endif

  SAMPLE_TDL_VPSS_CONFIG_S *pstVpssConfig =
      &pstMWConfig->stVPSSPoolConfig.astVpssConfig[0];
  pstVpssConfig->bBindVI = true;
  pstVpssConfig->u32ChnCount = 1;
  pstVpssConfig->u32ChnBindVI = 0;
  VPSS_GRP_DEFAULT_HELPER2(&pstVpssConfig->stVpssGrpAttr, stSensorSize.u32Width,
                           stSensorSize.u32Height, VI_PIXEL_FORMAT, 1);
  VPSS_CHN_DEFAULT_HELPER(&pstVpssConfig->astVpssChnAttr[0],
                          stVencSize.u32Width, stVencSize.u32Height,
                          VI_PIXEL_FORMAT, true);

  SAMPLE_TDL_Get_Input_Config(&pstMWConfig->stVencConfig.stChnInputCfg);
  std::snprintf(pstMWConfig->stVencConfig.stChnInputCfg.codec,
                sizeof(pstMWConfig->stVencConfig.stChnInputCfg.codec), "%s",
                codec.c_str());
  pstMWConfig->stVencConfig.u32FrameWidth = stVencSize.u32Width;
  pstMWConfig->stVencConfig.u32FrameHeight = stVencSize.u32Height;

  SAMPLE_TDL_Get_RTSP_Config(&pstMWConfig->stRTSPConfig.stRTSPConfig);
  return CVI_SUCCESS;
}

}  // namespace

int main(int argc, char *argv[]) {
  ProgramOptions options;
  if (parse_options(argc, argv, options) != CVI_SUCCESS) {
    return CVI_FAILURE;
  }

  std::signal(SIGINT, SampleHandleSig);
  std::signal(SIGTERM, SampleHandleSig);

  SAMPLE_TDL_MW_CONFIG_S stMWConfig = {};
  CVI_S32 s32Ret =
      get_middleware_config(&stMWConfig, options.vencSize, options.codec);
  if (s32Ret != CVI_SUCCESS) {
    std::printf("get middleware configuration failed! ret=%#x\n", s32Ret);
    return CVI_FAILURE;
  }

  Middleware middleware;
  s32Ret = middleware.init(&stMWConfig);
  if (s32Ret != CVI_SUCCESS) {
    std::printf("init middleware failed! ret=%#x\n", s32Ret);
    return CVI_FAILURE;
  }

  std::printf("RTSP stream is ready: rtsp://<duo-ip>/%s\n",
              options.codec.c_str());

  CVI_S32 rtspRet = CVI_SUCCESS;
  std::thread rtspThread;
  try {
    rtspThread = std::thread([&]() { rtspRet = run_rtsp(middleware.get()); });
  } catch (const std::system_error &error) {
    std::printf("create RTSP thread failed: %s\n", error.what());
    request_exit();
    return CVI_FAILURE;
  }

  rtspThread.join();
  return rtspRet == CVI_SUCCESS ? CVI_SUCCESS : CVI_FAILURE;
}
