#define LOG_TAG "SampleOD"
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
#include <string>
#include <system_error>
#include <thread>

namespace {

#ifdef CV180X
constexpr CVI_U32 k_stream_width = 1280;
constexpr CVI_U32 k_stream_height = 720;
#else
constexpr CVI_U32 k_stream_width = 1920;
constexpr CVI_U32 k_stream_height = 1080;
#endif
constexpr CVI_S32 k_frame_timeout_ms = 2000;

volatile std::sig_atomic_t g_signal_exit_requested = 0;
std::atomic<bool> g_thread_exit_requested(false);

struct ProgramOptions {
  std::string model_name;
  std::string model_path;
  bool threshold_set = false;
  float threshold = 0.5F;
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
  std::printf("sample_vi_od - Object detection sample\n\n");
  std::printf("Usage: %s MODEL_NAME MODEL_PATH [THRESHOLD]\n\n", program);
  std::printf("MODEL_NAME:\n"
              "\tmobiledetv2-person-vehicle  Person and vehicle detection (mobiledetv2)\n"
              "\tmobiledetv2-coco80          Coco 80 objects detection (mobiledetv2)\n"
              "\tmobiledetv2-vehicle         Vehicle detection (mobiledetv2)\n"
              "\tmobiledetv2-pedestrian      Pedestrian detection (mobiledetv2)\n"
              "\tyolov8-person-pets          Person, cat and dog detection (yolov8)\n"
              "\tyolov8n-det-person-vehicle  Person and vehicle detection (yolov8n)\n"
              "\tyolov8-coco80\n"
              "\tyolox\n\n");
  std::printf("MODEL_PATH: cvimodel path\n\n");
  std::printf("THRESHOLD: (optional) threshold for detection model (default: 0.5)\n\n");
}

static bool parse_threshold(const char *text, float &threshold) {
  if (text == nullptr || *text == '\0') {
    return false;
  }

  char *end = nullptr;
  const float parsed = std::strtof(text, &end);
  if (end == text || *end != '\0' || parsed < 0.0F || parsed > 1.0F) {
    return false;
  }

  threshold = parsed;
  return true;
}

static CVI_S32 parse_options(int argc, char *argv[], ProgramOptions &options) {
  if (argc != 3 && argc != 4) {
    print_usage(argv[0]);
    return CVI_FAILURE;
  }

  options.model_name = argv[1];
  options.model_path = argv[2];

  if (argc == 4) {
    if (!parse_threshold(argv[3], options.threshold)) {
      std::printf("wrong threshold value: %s\n", argv[3]);
      return CVI_FAILURE;
    }
    options.threshold_set = true;
  }

  return CVI_SUCCESS;
}

static cvi_media::PipelineConfig make_pipeline_config() {
  cvi_media::PipelineConfig config;
  config.stream_size = cvi_media::Size{k_stream_width, k_stream_height};
  config.codec = cvi_media::Codec::H264;
  config.stream_channel = VPSS_CHN0;

  config.vpss_channels.push_back(cvi_media::make_vpss_channel(VPSS_CHN0, config.stream_size));
  config.vpss_channels.push_back(cvi_media::make_vpss_channel(VPSS_CHN1, config.stream_size));

  cvi_media::VbPoolConfig preprocess_pool;
  preprocess_pool.pixel_format = PIXEL_FORMAT_RGB_888_PLANAR;
  preprocess_pool.block_count = 1;
#ifdef CV180X
  preprocess_pool.size = cvi_media::Size{384, 256};
#else
  preprocess_pool.size = cvi_media::Size{1024, 768};
#endif
  config.extra_vb_pools.push_back(preprocess_pool);

  return config;
}

static void print_pet_person_objects(const cvi_media::tdl::ObjectMeta &meta) {
  const TDLObject *object = meta.get();
  for (uint32_t i = 0; i < object->size; ++i) {
    const uint32_t class_id = object->info[i].class_id;
    const char *label = "";
    if (class_id == 0) {
      label = "cat:";
    } else if (class_id == 1) {
      label = "dog:";
    } else if (class_id == 2) {
      label = "person:";
    }

    std::printf("%-7s %.2f %.2f %.2f %.2f %d %.2f\n", label,
                object->info[i].box.x1, object->info[i].box.y1,
                object->info[i].box.x2, object->info[i].box.y2,
                object->info[i].class_id, object->info[i].score);
  }
}

static CVI_S32 run_venc(cvi_media::MediaPipeline &pipeline,
                        cvi_media::tdl::ObjectStore &objects,
                        TDLModel model) {
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
      if (model == TDL_MODEL_YOLOV8N_DET_PET_PERSON) {
        object_meta.label_pet_person();
      } else {
        object_meta.label_class_ids();
      }
      object_meta.draw(frame, true);
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
                       const cvi_media::tdl::ObjectModelInfo &model_info,
                       cvi_media::tdl::ObjectStore &objects) {
  std::printf("Enter TDL thread\n");

  CVI_S32 result = CVI_SUCCESS;
  uint32_t counter = 0;

  while (!exit_requested()) {
    try {
      cvi_media::VideoFrame frame =
          pipeline.acquire_frame(0, VPSS_CHN1, k_frame_timeout_ms);
      if (exit_requested()) {
        break;
      }

      const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
      cvi_media::tdl::ObjectMeta object_meta = cvi_media::tdl::detect_objects(
          tdl_handle, model_info.model, model_info.inference, frame);
      const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();

      if (model_info.model == TDL_MODEL_YOLOV8N_DET_PET_PERSON) {
        print_pet_person_objects(object_meta);
      } else if (counter++ % 5 == 0) {
        const auto elapsed_us =
            std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        std::printf("obj count: %u, take %.2f ms, width:%u\n", object_meta.size(),
                    static_cast<double>(elapsed_us) / 1000.0,
                    frame.get()->stVFrame.u32Width);
      }

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

  cvi_media::tdl::ObjectModelInfo model_info;
  try {
    model_info = cvi_media::tdl::object_model_from_name(options.model_name);
  } catch (const cvi_media::tdl::TdlError &error) {
    std::printf("unsupported model: %s\n", options.model_name.c_str());
    return CVI_FAILURE;
  }

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
    tdl_handle->open_model(model_info.model, options.model_path.c_str(), nullptr);
    if (options.threshold_set) {
      std::printf("set threshold to %f\n", options.threshold);
      tdl_handle->set_threshold(model_info.model, options.threshold);
    }
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
    venc_thread = std::thread([&]() { venc_ret = run_venc(pipeline, objects, model_info.model); });
    tdl_thread = std::thread([&]() { tdl_ret = run_tdl(pipeline, *tdl_handle, model_info, objects); });
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
