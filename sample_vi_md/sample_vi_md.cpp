#define LOG_TAG "SampleMD"
#define LOG_LEVEL LOG_LEVEL_INFO

#include "cvi_media/media_pipeline.hpp"
#include "cvi_media/tdl_pipeline.hpp"

#include <cvi_comm.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <system_error>
#include <thread>

namespace {

constexpr CVI_U32 k_stream_width = 1280;
constexpr CVI_U32 k_stream_height = 720;
constexpr CVI_S32 k_frame_timeout_ms = 2000;
constexpr CVI_U32 k_background_update_interval = 2;

volatile std::sig_atomic_t g_signal_exit_requested = 0;
std::atomic<bool> g_thread_exit_requested(false);

struct ProgramOptions {
  CVI_U8 threshold = 0;
  CVI_FLOAT min_area = 0.0;
};

static bool exit_requested() {
  return g_signal_exit_requested != 0 ||
         g_thread_exit_requested.load(std::memory_order_relaxed);
}

static void request_thread_exit() {
  g_thread_exit_requested.store(true, std::memory_order_relaxed);
}

static void exit_sig_handler(int signo) {
  if (SIGINT == signo || SIGTERM == signo) {
    g_signal_exit_requested = 1;
  }
}

static void print_usage(const char *program) {
  std::printf(
      "\nUsage: %s THRESHOLD MIN_AREA\n\n"
      "\tTHRESHOLD, threshold for motion detection [0-255].\n"
      "\tMIN_AREA, minimal pixel size of object.\n\n",
      program);
}

static bool parse_u8(const char *text, CVI_U8 &value) {
  if (text == nullptr || *text == '\0' || *text == '-') {
    return false;
  }

  char *end = nullptr;
  const unsigned long parsed = std::strtoul(text, &end, 10);
  if (end == text || *end != '\0' || parsed > 255UL) {
    return false;
  }

  value = static_cast<CVI_U8>(parsed);
  return true;
}

static bool parse_float(const char *text, CVI_FLOAT &value) {
  if (text == nullptr || *text == '\0') {
    return false;
  }

  char *end = nullptr;
  const double parsed = std::strtod(text, &end);
  if (end == text || *end != '\0' || parsed < 0.0) {
    return false;
  }

  value = static_cast<CVI_FLOAT>(parsed);
  return true;
}

static CVI_S32 parse_options(int argc, char *argv[], ProgramOptions &options) {
  if (argc != 3) {
    print_usage(argv[0]);
    return CVI_FAILURE;
  }

  if (!parse_u8(argv[1], options.threshold)) {
    std::printf("invalid threshold: %s\n", argv[1]);
    return CVI_FAILURE;
  }

  if (!parse_float(argv[2], options.min_area)) {
    std::printf("invalid min area: %s\n", argv[2]);
    return CVI_FAILURE;
  }

  return CVI_SUCCESS;
}

static cvi_media::PipelineConfig make_pipeline_config() {
  cvi_media::PipelineConfig config;
  config.stream_size = cvi_media::Size{k_stream_width, k_stream_height};
  config.codec = cvi_media::Codec::H264;
  config.stream_channel = VPSS_CHN0;

  config.vpss_channels.push_back(cvi_media::make_vpss_channel(VPSS_CHN0, config.stream_size));
  config.vpss_channels.push_back(
      cvi_media::make_vpss_channel(VPSS_CHN1, config.stream_size, PIXEL_FORMAT_YUV_400));

  return config;
}

static CVI_S32 run_venc(cvi_media::MediaPipeline &pipeline,
                        cvi_media::tdl::ObjectStore &objects) {
  std::printf("Enter encoder thread\n");

  CVI_S32 result = CVI_SUCCESS;
  while (!exit_requested()) {
    try {
      cvi_media::VideoFrame frame =
          pipeline.acquire_frame(0, VPSS_CHN0, k_frame_timeout_ms);
      if (exit_requested()) {
        break;
      }

      cvi_media::tdl::ObjectMeta object_meta = objects.snapshot();
      object_meta.draw(frame, false);
      pipeline.send_frame(frame);
    } catch (const cvi_media::CviError &error) {
      if (!exit_requested()) {
        std::printf("%s\n", error.what());
        request_thread_exit();
        result = error.code();
      }
      break;
    } catch (const cvi_media::tdl::TdlError &error) {
      if (!exit_requested()) {
        std::printf("%s\n", error.what());
        request_thread_exit();
        result = error.code();
      }
      break;
    } catch (const std::exception &error) {
      if (!exit_requested()) {
        std::printf("encoder thread failed: %s\n", error.what());
        request_thread_exit();
        result = CVI_FAILURE;
      }
      break;
    }
  }

  std::printf("Exit encoder thread\n");
  return result;
}

static CVI_S32 run_tdl(cvi_media::MediaPipeline &pipeline,
                       cvi_media::tdl::Handle &tdl_handle,
                       const ProgramOptions &options,
                       cvi_media::tdl::ObjectStore &objects) {
  std::printf("Enter TDL thread\n");

  CVI_S32 result = CVI_SUCCESS;
  uint32_t frame_count = 0;

  while (!exit_requested()) {
    try {
      cvi_media::VideoFrame frame =
          pipeline.acquire_frame(0, VPSS_CHN1, k_frame_timeout_ms);
      if (exit_requested()) {
        break;
      }

      const CVI_U32 update_interval =
          (frame_count == 0) ? 0 : k_background_update_interval;
      const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
      cvi_media::tdl::ObjectMeta object_meta =
          cvi_media::tdl::detect_motion(tdl_handle, frame, options.threshold,
                                        options.min_area, update_interval);
      const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();

      ++frame_count;
      const auto elapsed_us =
          std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
      std::printf("detected objects: %u, time= %.2f ms\n", object_meta.size(),
                  static_cast<double>(elapsed_us) / 1000.0);

      objects.update(object_meta);
    } catch (const cvi_media::CviError &error) {
      if (!exit_requested()) {
        std::printf("%s\n", error.what());
        request_thread_exit();
        result = error.code();
      }
      break;
    } catch (const cvi_media::tdl::TdlError &error) {
      if (!exit_requested()) {
        std::printf("%s\n", error.what());
        request_thread_exit();
        result = error.code();
      }
      break;
    } catch (const std::exception &error) {
      if (!exit_requested()) {
        std::printf("TDL thread failed: %s\n", error.what());
        request_thread_exit();
        result = CVI_FAILURE;
      }
      break;
    }
  }

  std::printf("Exit TDL thread\n");
  return result;
}

}  // namespace

int main(int argc, char *argv[]) {
  ProgramOptions options;
  if (parse_options(argc, argv, options) != CVI_SUCCESS) {
    return CVI_FAILURE;
  }

  std::signal(SIGINT, exit_sig_handler);
  std::signal(SIGTERM, exit_sig_handler);

  cvi_media::MediaPipeline pipeline;
  try {
    pipeline.start(make_pipeline_config());
  } catch (const cvi_media::CviError &error) {
    std::printf("init media pipeline failed: %s\n", error.what());
    return CVI_FAILURE;
  }

  std::unique_ptr<cvi_media::tdl::Handle> tdl_handle;
  try {
    tdl_handle.reset(new cvi_media::tdl::Handle());
  } catch (const cvi_media::tdl::TdlError &error) {
    std::printf("setup TDL failed: %s\n", error.what());
    return CVI_FAILURE;
  }

  std::printf("RTSP stream is ready: rtsp://<duo-ip>/%s\n",
              cvi_media::codec_name(cvi_media::Codec::H264));

  cvi_media::tdl::ObjectStore objects;
  CVI_S32 venc_ret = CVI_SUCCESS;
  CVI_S32 tdl_ret = CVI_SUCCESS;
  std::thread venc_thread;
  std::thread tdl_thread;

  try {
    venc_thread = std::thread([&]() { venc_ret = run_venc(pipeline, objects); });
    tdl_thread = std::thread([&]() { tdl_ret = run_tdl(pipeline, *tdl_handle, options, objects); });
  } catch (const std::system_error &error) {
    std::printf("create worker thread failed: %s\n", error.what());
    request_thread_exit();
    if (venc_thread.joinable()) {
      venc_thread.join();
    }
    if (tdl_thread.joinable()) {
      tdl_thread.join();
    }
    return CVI_FAILURE;
  }

  venc_thread.join();
  request_thread_exit();
  tdl_thread.join();

  return (venc_ret == CVI_SUCCESS && tdl_ret == CVI_SUCCESS) ? CVI_SUCCESS : CVI_FAILURE;
}
