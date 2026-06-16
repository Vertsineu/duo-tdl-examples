#define LOG_TAG "SampleVIRtsp"
#define LOG_LEVEL LOG_LEVEL_INFO

#include "cvi_media/media_pipeline.hpp"
#include "cvi_media/parse.hpp"

#include <cvi_comm.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <getopt.h>
#include <string>
#include <system_error>
#include <thread>

namespace {

constexpr CVI_U32 k_default_width = 1280;
constexpr CVI_U32 k_default_height = 720;
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

struct ProgramOptions {
  cvi_media::Size stream_size{k_default_width, k_default_height};
  cvi_media::Codec codec = cvi_media::Codec::H264;
  bool help_requested = false;
};

static void exit_sig_handler(int signo) {
  if (SIGINT == signo || SIGTERM == signo) {
    g_signal_exit_requested = 1;
  }
}

static void print_usage(const char *program) {
  std::printf("\nUsage:\n");
  std::printf("  %s [WIDTH HEIGHT [h264|h265]]\n", program);
  std::printf("  %s --width WIDTH --height HEIGHT [--codec h264|h265]\n\n",
              program);
  std::printf("Default: %s %u %u %s\n", program, k_default_width, k_default_height,
              cvi_media::codec_name(cvi_media::Codec::H264));
  std::printf("RTSP URL: rtsp://<duo-ip>/%s\n\n",
              cvi_media::codec_name(cvi_media::Codec::H264));
  std::printf("Options:\n");
  std::printf("  -w, --width WIDTH      Output width\n");
  std::printf("  -H, --height HEIGHT    Output height\n");
  std::printf("  -c, --codec CODEC      h264 or h265\n");
  std::printf("      --help             Show this help\n\n");
}

static CVI_S32 parse_options(int argc, char *argv[], ProgramOptions &options) {
  bool width_set = false;
  bool height_set = false;
  bool codec_set = false;

  enum LongOption {
    k_help = 1000,
  };

  const option long_options[] = {
      {"width", required_argument, nullptr, 'w'},
      {"height", required_argument, nullptr, 'H'},
      {"codec", required_argument, nullptr, 'c'},
      {"help", no_argument, nullptr, k_help},
      {nullptr, 0, nullptr, 0},
  };

  optind = 1;
  opterr = 0;
  for (;;) {
    const int opt = getopt_long(argc, argv, "w:H:c:", long_options, nullptr);
    if (opt == -1) {
      break;
    }

    switch (opt) {
    case 'w':
      if (!cvi_media::parse_arg<CVI_U32>(optarg, options.stream_size.width)) {
        std::printf("Invalid width: %s\n", optarg);
        return CVI_FAILURE;
      }
      width_set = true;
      break;
    case 'H':
      if (!cvi_media::parse_arg<CVI_U32>(optarg, options.stream_size.height)) {
        std::printf("Invalid height: %s\n", optarg);
        return CVI_FAILURE;
      }
      height_set = true;
      break;
    case 'c':
      if (!cvi_media::parse_arg<cvi_media::Codec>(optarg, options.codec)) {
        std::printf("Unsupported codec: %s. Use h264 or h265.\n", optarg);
        return CVI_FAILURE;
      }
      codec_set = true;
      break;
    case k_help:
      options.help_requested = true;
      print_usage(argv[0]);
      return CVI_SUCCESS;
    default:
      std::printf("Invalid option. Use --help for usage.\n");
      return CVI_FAILURE;
    }
  }

  const int positional_count = argc - optind;
  if (positional_count != 0 && positional_count != 2 && positional_count != 3) {
    print_usage(argv[0]);
    return CVI_FAILURE;
  }

  if (positional_count >= 2) {
    if (width_set || height_set) {
      std::printf(
          "Do not mix positional width/height with --width/--height.\n");
      return CVI_FAILURE;
    }

    if (!cvi_media::parse_arg<CVI_U32>(argv[optind], options.stream_size.width) ||
        !cvi_media::parse_arg<CVI_U32>(argv[optind + 1], options.stream_size.height)) {
      std::printf("Invalid width or height.\n");
      return CVI_FAILURE;
    }
  }

  if (positional_count == 3) {
    if (codec_set) {
      std::printf("Do not mix positional codec with --codec.\n");
      return CVI_FAILURE;
    }

    if (!cvi_media::parse_arg<cvi_media::Codec>(argv[optind + 2], options.codec)) {
      std::printf("Unsupported codec: %s. Use h264 or h265.\n",
                  argv[optind + 2]);
      return CVI_FAILURE;
    }
  }

  if (cvi_media::pic_size_from_dimensions(options.stream_size) == PIC_BUTT) {
    std::printf("Unsupported frame size: %ux%u. Try 1280x720 or 1920x1080.\n",
                options.stream_size.width, options.stream_size.height);
    return CVI_FAILURE;
  }

  return CVI_SUCCESS;
}

static CVI_S32 run_rtsp(cvi_media::MediaPipeline &pipeline) {
  std::printf("Enter RTSP thread\n");

  CVI_S32 result = CVI_SUCCESS;
  while (!exit_requested()) {
    try {
      cvi_media::VideoFrame frame = pipeline.acquire_frame(
          static_cast<VPSS_GRP>(0), VPSS_CHN0, k_frame_timeout_ms);
      if (exit_requested()) {
        break;
      }
      pipeline.send_frame(frame);
    } catch (const cvi_media::CviError &error) {
      if (exit_requested()) {
        break;
      }
      std::printf("%s\n", error.what());
      request_thread_exit();
      result = error.code();
      break;
    }
  }

  std::printf("Exit RTSP thread\n");
  return result;
}

} // namespace

int main(int argc, char *argv[]) {
  ProgramOptions options;
  if (parse_options(argc, argv, options) != CVI_SUCCESS) {
    return CVI_FAILURE;
  }
  if (options.help_requested) {
    return CVI_SUCCESS;
  }

  std::signal(SIGINT, exit_sig_handler);
  std::signal(SIGTERM, exit_sig_handler);

  cvi_media::PipelineConfig config;
  config.stream_size = options.stream_size;
  config.codec = options.codec;

  cvi_media::MediaPipeline pipeline;
  try {
    pipeline.start(config);
  } catch (const cvi_media::CviError &error) {
    std::printf("init media pipeline failed: %s\n", error.what());
    return CVI_FAILURE;
  }

  std::printf("RTSP stream is ready: rtsp://<duo-ip>/%s\n",
              cvi_media::codec_name(options.codec));

  CVI_S32 rtsp_ret = CVI_SUCCESS;
  std::thread rtsp_thread;
  try {
    rtsp_thread = std::thread([&]() { rtsp_ret = run_rtsp(pipeline); });
  } catch (const std::system_error &error) {
    std::printf("create RTSP thread failed: %s\n", error.what());
    request_thread_exit();
    return CVI_FAILURE;
  }

  rtsp_thread.join();
  return rtsp_ret == CVI_SUCCESS ? CVI_SUCCESS : CVI_FAILURE;
}
