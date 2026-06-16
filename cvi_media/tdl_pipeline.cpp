#include "tdl_pipeline.hpp"

#include <tdl_utils.h>

#include <cstdio>
#include <cstring>
#include <utility>

namespace cvi_media {
namespace tdl {
namespace {

std::string make_tdl_error_message(const std::string &operation, CVI_S32 code) {
  char buffer[256];
  std::snprintf(buffer, sizeof(buffer), "%s failed with %#x", operation.c_str(), code);
  return buffer;
}

void throw_if_tdl_failed(CVI_S32 code, const char *operation) {
  if (code != CVI_SUCCESS) {
    throw TdlError(operation, code);
  }
}

bool erase_model(std::vector<TDLModel> &models, TDLModel model) {
  for (std::vector<TDLModel>::iterator it = models.begin(); it != models.end(); ++it) {
    if (*it == model) {
      models.erase(it);
      return true;
    }
  }
  return false;
}

}  // namespace

TdlError::TdlError(const std::string &operation, CVI_S32 code)
    : std::runtime_error(make_tdl_error_message(operation, code)), code_(code) {}

Image::Image(TDLImage image) noexcept : image_(image) {}

Image::Image(Image &&other) noexcept { *this = std::move(other); }

Image &Image::operator=(Image &&other) noexcept {
  if (this != &other) {
    reset();
    image_ = other.image_;
    other.image_ = nullptr;
  }
  return *this;
}

Image::~Image() { reset(); }

Image Image::wrap(VideoFrame &frame, bool own_memory) {
  TDLImage image = TDL_WrapFrame(frame.get(), own_memory);
  if (image == nullptr) {
    throw TdlError("TDL_WrapFrame", CVI_FAILURE);
  }
  return Image(image);
}

void Image::reset() noexcept {
  if (image_ != nullptr) {
    TDL_DestroyImage(image_);
    image_ = nullptr;
  }
}

Handle::Handle(int32_t tpu_device_id) : handle_(TDL_CreateHandle(tpu_device_id)) {
  if (handle_ == nullptr) {
    throw TdlError("TDL_CreateHandle", CVI_FAILURE);
  }
}

Handle::Handle(Handle &&other) noexcept { *this = std::move(other); }

Handle &Handle::operator=(Handle &&other) noexcept {
  if (this != &other) {
    destroy();
    handle_ = other.handle_;
    open_models_ = std::move(other.open_models_);
    other.handle_ = nullptr;
    other.open_models_.clear();
  }
  return *this;
}

Handle::~Handle() { destroy(); }

void Handle::open_model(TDLModel model, const char *model_path, const char *model_config_json) {
  throw_if_tdl_failed(TDL_OpenModel(handle_, model, model_path, model_config_json),
                      "TDL_OpenModel");
  open_models_.push_back(model);
}

void Handle::close_model(TDLModel model) noexcept {
  if (handle_ != nullptr && erase_model(open_models_, model)) {
    TDL_CloseModel(handle_, model);
  }
}

void Handle::set_threshold(TDLModel model, float threshold) {
  throw_if_tdl_failed(TDL_SetModelThreshold(handle_, model, threshold), "TDL_SetModelThreshold");
}

void Handle::destroy() noexcept {
  if (handle_ == nullptr) {
    return;
  }
  for (std::vector<TDLModel>::reverse_iterator it = open_models_.rbegin();
       it != open_models_.rend(); ++it) {
    TDL_CloseModel(handle_, *it);
  }
  open_models_.clear();
  TDL_DestroyHandle(handle_);
  handle_ = nullptr;
}

FaceMeta::FaceMeta() noexcept { std::memset(&meta_, 0, sizeof(meta_)); }

FaceMeta::FaceMeta(const FaceMeta &other) : FaceMeta() {
  if (other.meta_.info != nullptr && other.meta_.size > 0) {
    throw_if_tdl_failed(TDL_CopyFaceMeta(other.get(), &meta_), "TDL_CopyFaceMeta");
  }
}

FaceMeta &FaceMeta::operator=(const FaceMeta &other) {
  if (this != &other) {
    clear();
    if (other.meta_.info != nullptr && other.meta_.size > 0) {
      throw_if_tdl_failed(TDL_CopyFaceMeta(other.get(), &meta_), "TDL_CopyFaceMeta");
    }
  }
  return *this;
}

FaceMeta::FaceMeta(FaceMeta &&other) noexcept { *this = std::move(other); }

FaceMeta &FaceMeta::operator=(FaceMeta &&other) noexcept {
  if (this != &other) {
    clear();
    meta_ = other.meta_;
    std::memset(&other.meta_, 0, sizeof(other.meta_));
  }
  return *this;
}

FaceMeta::~FaceMeta() { clear(); }

void FaceMeta::clear() noexcept {
  TDL_ReleaseFaceMeta(&meta_);
  std::memset(&meta_, 0, sizeof(meta_));
}

void FaceMeta::draw(VideoFrame &frame, bool draw_text) const {
  throw_if_tdl_failed(draw_face_rect(meta_, *frame.get(), draw_text), "draw_face_rect");
}

ObjectMeta::ObjectMeta() noexcept { std::memset(&meta_, 0, sizeof(meta_)); }

ObjectMeta::ObjectMeta(const ObjectMeta &other) : ObjectMeta() {
  if (other.meta_.info != nullptr && other.meta_.size > 0) {
    throw_if_tdl_failed(TDL_CopyObjectMeta(other.get(), &meta_), "TDL_CopyObjectMeta");
  }
}

ObjectMeta &ObjectMeta::operator=(const ObjectMeta &other) {
  if (this != &other) {
    clear();
    if (other.meta_.info != nullptr && other.meta_.size > 0) {
      throw_if_tdl_failed(TDL_CopyObjectMeta(other.get(), &meta_), "TDL_CopyObjectMeta");
    }
  }
  return *this;
}

ObjectMeta::ObjectMeta(ObjectMeta &&other) noexcept { *this = std::move(other); }

ObjectMeta &ObjectMeta::operator=(ObjectMeta &&other) noexcept {
  if (this != &other) {
    clear();
    meta_ = other.meta_;
    std::memset(&other.meta_, 0, sizeof(other.meta_));
  }
  return *this;
}

ObjectMeta::~ObjectMeta() { clear(); }

void ObjectMeta::clear() noexcept {
  TDL_ReleaseObjectMeta(&meta_);
  std::memset(&meta_, 0, sizeof(meta_));
}

void ObjectMeta::draw(VideoFrame &frame, bool draw_text) const {
  throw_if_tdl_failed(draw_object_rect(meta_, *frame.get(), draw_text), "draw_object_rect");
}

void ObjectMeta::label_class_ids() {
  for (uint32_t i = 0; i < meta_.size; ++i) {
    std::snprintf(meta_.info[i].name, sizeof(meta_.info[i].name), "class:%d %.2f",
                  meta_.info[i].class_id, meta_.info[i].score);
  }
}

void ObjectMeta::label_pet_person() {
  for (uint32_t i = 0; i < meta_.size; ++i) {
    const char *label = "";
    if (meta_.info[i].class_id == 0) {
      label = "cat";
    } else if (meta_.info[i].class_id == 1) {
      label = "dog";
    } else if (meta_.info[i].class_id == 2) {
      label = "person";
    }
    std::snprintf(meta_.info[i].name, sizeof(meta_.info[i].name), "%s: %.2f", label,
                  meta_.info[i].score);
  }
}

void FaceStore::update(const FaceMeta &meta) {
  std::lock_guard<std::mutex> lock(mutex_);
  latest_ = meta;
}

FaceMeta FaceStore::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return latest_;
}

FaceMeta FaceStore::take() {
  std::lock_guard<std::mutex> lock(mutex_);
  FaceMeta result = latest_;
  latest_.clear();
  return result;
}

void ObjectStore::update(const ObjectMeta &meta) {
  std::lock_guard<std::mutex> lock(mutex_);
  latest_ = meta;
}

ObjectMeta ObjectStore::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return latest_;
}

ObjectMeta ObjectStore::take() {
  std::lock_guard<std::mutex> lock(mutex_);
  ObjectMeta result = latest_;
  latest_.clear();
  return result;
}

ObjectModelInfo object_model_from_name(const std::string &name) {
  ObjectModelInfo info;
  info.inference = TDL_Detection;

  if (name == "mbv2-det-person" || name == "mobiledetv2-pedestrian") {
    info.model = TDL_MODEL_MBV2_DET_PERSON;
  } else if (name == "yolov8-person-pets" || name == "yolov8n-det-pet-person" ||
             name == "yolov8n_det_pet_person") {
    info.model = TDL_MODEL_YOLOV8N_DET_PET_PERSON;
  } else if (name == "yolov8n-det-person-vehicle" ||
             name == "yolov8n_det_person_vehicle" ||
             name == "mobiledetv2-person-vehicle" || name == "mobiledetv2-vehicle") {
    info.model = TDL_MODEL_YOLOV8N_DET_PERSON_VEHICLE;
  } else if (name == "yolov8-coco80" || name == "mobiledetv2-coco80") {
    info.model = TDL_MODEL_YOLOV8_DET_COCO80;
  } else if (name == "yolox" || name == "yolox-coco80") {
    info.model = TDL_MODEL_YOLOX_DET_COCO80;
  } else {
    throw TdlError("unsupported object model name: " + name, CVI_FAILURE);
  }

  return info;
}

FaceMeta detect_faces(Handle &handle, TDLModel model, VideoFrame &frame) {
  FaceMeta result;
  Image image = Image::wrap(frame, false);
  throw_if_tdl_failed(TDL_FaceDetection(handle.get(), model, image.get(), result.get()),
                      "TDL_FaceDetection");
  return result;
}

ObjectMeta detect_objects(Handle &handle, TDLModel model, ObjectInferenceFunc inference,
                          VideoFrame &frame) {
  if (inference == nullptr) {
    throw TdlError("null object inference function", CVI_FAILURE);
  }

  ObjectMeta result;
  Image image = Image::wrap(frame, false);
  throw_if_tdl_failed(inference(handle.get(), model, image.get(), result.get()),
                      "object inference");
  return result;
}

ObjectMeta detect_motion(Handle &handle, VideoFrame &frame, CVI_U8 threshold,
                         CVI_FLOAT min_area, CVI_U32 update_interval) {
#if defined(__CV181X__) || defined(__CV184X__) || defined(__CV186X__)
  ObjectMeta result;
  Image image = Image::wrap(frame, false);

  TDLObjectInfo roi_info;
  std::memset(&roi_info, 0, sizeof(roi_info));
  roi_info.box.x1 = 0;
  roi_info.box.y1 = 0;
  roi_info.box.x2 = static_cast<float>(frame.get()->stVFrame.u32Width);
  roi_info.box.y2 = static_cast<float>(frame.get()->stVFrame.u32Height);

  TDLObject roi;
  std::memset(&roi, 0, sizeof(roi));
  roi.size = 1;
  roi.width = frame.get()->stVFrame.u32Width;
  roi.height = frame.get()->stVFrame.u32Height;
  roi.info = &roi_info;

  throw_if_tdl_failed(TDL_MotionDetection(handle.get(), image.get(), image.get(), &roi, threshold,
                                          min_area, result.get(), update_interval),
                      "TDL_MotionDetection");
  return result;
#else
  (void)handle;
  (void)frame;
  (void)threshold;
  (void)min_area;
  (void)update_interval;
  throw TdlError("TDL_MotionDetection is not supported on this chip", CVI_FAILURE);
#endif
}

}  // namespace tdl
}  // namespace cvi_media
