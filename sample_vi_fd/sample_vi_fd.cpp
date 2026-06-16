#define LOG_TAG "SampleFD"
#define LOG_LEVEL LOG_LEVEL_INFO

#include "cvi_media/media_pipeline.hpp"
#include "cvi_media/tdl_pipeline.hpp"

#include <cvi_comm.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <exception>
#include <system_error>
#include <thread>

namespace {

constexpr CVI_U32 k_stream_width = 1280;
constexpr CVI_U32 k_stream_height = 720;
constexpr CVI_S32 k_frame_timeout_ms = 2000;

volatile std::sig_atomic_t g_signal_exit_requested = 0;
std::atomic<bool> g_thread_exit_requested(false);

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
      "\nUsage: %s SCRFDFACE_MODEL_PATH.\n\n"
      "\tSCRFDFACE_MODEL_PATH, path to scrfdface model.\n\n",
      program);
}

static cvi_media::PipelineConfig make_pipeline_config() {
  cvi_media::PipelineConfig config;
  config.stream_size = cvi_media::Size{k_stream_width, k_stream_height};
  config.codec = cvi_media::Codec::H264;
  config.stream_channel = VPSS_CHN0;

  cvi_media::VpssChannelConfig stream_channel =
      cvi_media::make_vpss_channel(VPSS_CHN0, config.stream_size);
  cvi_media::VpssChannelConfig tdl_channel =
      cvi_media::make_vpss_channel(VPSS_CHN1, config.stream_size);
  config.vpss_channels.push_back(stream_channel);
  config.vpss_channels.push_back(tdl_channel);

  cvi_media::VbPoolConfig preprocess_pool;
  preprocess_pool.pixel_format = PIXEL_FORMAT_BGR_888_PLANAR;
#ifdef CV180X
  preprocess_pool.block_count = 1;
  preprocess_pool.size = cvi_media::Size{320, 256};
#else
  preprocess_pool.block_count = 3;
  preprocess_pool.size = cvi_media::Size{1280, 720};
#endif
  config.extra_vb_pools.push_back(preprocess_pool);

  return config;
}

static CVI_S32 run_venc(cvi_media::MediaPipeline &pipeline,
                        const cvi_media::tdl::FaceStore &faces) {
  std::printf("Enter encoder thread\n");

  CVI_S32 result = CVI_SUCCESS;
  while (!exit_requested()) {
    try {
      cvi_media::VideoFrame frame =
          pipeline.acquire_frame(0, VPSS_CHN0, k_frame_timeout_ms);
      if (exit_requested()) {
        break;
      }

      cvi_media::tdl::FaceMeta face_meta = faces.snapshot();
      face_meta.draw(frame, false);
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
                       cvi_media::tdl::FaceStore &faces) {
  std::printf("Enter TDL thread\n");

  CVI_S32 result = CVI_SUCCESS;
  uint32_t last_face_count = 0;
  bool have_face_count = false;

  while (!exit_requested()) {
    try {
      cvi_media::VideoFrame frame =
          pipeline.acquire_frame(0, VPSS_CHN1, k_frame_timeout_ms);
      if (exit_requested()) {
        break;
      }

      cvi_media::tdl::FaceMeta face_meta =
          cvi_media::tdl::detect_faces(tdl_handle, TDL_MODEL_SCRFD_DET_FACE, frame);

      if (!have_face_count || face_meta.size() != last_face_count) {
        std::printf("face count: %u\n", face_meta.size());
        last_face_count = face_meta.size();
        have_face_count = true;
      }

      faces.update(face_meta);
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
  if (argc != 2) {
    print_usage(argv[0]);
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

  cvi_media::tdl::Handle tdl_handle;
  try {
    tdl_handle.open_model(TDL_MODEL_SCRFD_DET_FACE, argv[1], nullptr);
  } catch (const cvi_media::tdl::TdlError &error) {
    std::printf("setup TDL failed: %s\n", error.what());
    return CVI_FAILURE;
  }

  std::printf("RTSP stream is ready: rtsp://<duo-ip>/%s\n",
              cvi_media::codec_name(cvi_media::Codec::H264));

  cvi_media::tdl::FaceStore faces;
  CVI_S32 venc_ret = CVI_SUCCESS;
  CVI_S32 tdl_ret = CVI_SUCCESS;
  std::thread venc_thread;
  std::thread tdl_thread;

  try {
    venc_thread = std::thread([&]() { venc_ret = run_venc(pipeline, faces); });
    tdl_thread = std::thread([&]() { tdl_ret = run_tdl(pipeline, tdl_handle, faces); });
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
