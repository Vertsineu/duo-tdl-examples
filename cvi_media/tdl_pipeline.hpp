#pragma once

#include "media_pipeline.hpp"
#include "tdl_draw.hpp"

#include <tdl_sdk.h>
#include <tdl_types.h>

#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace cvi_media {
namespace tdl {

class TdlError : public std::runtime_error {
 public:
  TdlError(const std::string &operation, CVI_S32 code);

  CVI_S32 code() const noexcept { return code_; }

 private:
  CVI_S32 code_;
};

class Image {
 public:
  Image() noexcept = default;
  explicit Image(TDLImage image) noexcept;
  Image(const Image &) = delete;
  Image &operator=(const Image &) = delete;
  Image(Image &&other) noexcept;
  Image &operator=(Image &&other) noexcept;
  ~Image();

  static Image wrap(VideoFrame &frame, bool own_memory = false);

  TDLImage get() const noexcept { return image_; }
  explicit operator bool() const noexcept { return image_ != nullptr; }
  void reset() noexcept;

 private:
  TDLImage image_ = nullptr;
};

class Handle {
 public:
  explicit Handle(int32_t tpu_device_id = 0);
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  Handle(Handle &&other) noexcept;
  Handle &operator=(Handle &&other) noexcept;
  ~Handle();

  void open_model(TDLModel model, const char *model_path,
                  const char *model_config_json = nullptr);
  void close_model(TDLModel model) noexcept;
  void set_threshold(TDLModel model, float threshold);

  TDLHandle get() const noexcept { return handle_; }

 private:
  void destroy() noexcept;

  TDLHandle handle_ = nullptr;
  std::vector<TDLModel> open_models_;
};

class FaceMeta {
 public:
  FaceMeta() noexcept;
  FaceMeta(const FaceMeta &other);
  FaceMeta &operator=(const FaceMeta &other);
  FaceMeta(FaceMeta &&other) noexcept;
  FaceMeta &operator=(FaceMeta &&other) noexcept;
  ~FaceMeta();

  TDLFace *get() noexcept { return &meta_; }
  const TDLFace *get() const noexcept { return &meta_; }
  uint32_t size() const noexcept { return meta_.size; }
  void clear() noexcept;
  void draw(VideoFrame &frame, bool draw_text = false) const;

 private:
  TDLFace meta_{};
};

class ObjectMeta {
 public:
  ObjectMeta() noexcept;
  ObjectMeta(const ObjectMeta &other);
  ObjectMeta &operator=(const ObjectMeta &other);
  ObjectMeta(ObjectMeta &&other) noexcept;
  ObjectMeta &operator=(ObjectMeta &&other) noexcept;
  ~ObjectMeta();

  TDLObject *get() noexcept { return &meta_; }
  const TDLObject *get() const noexcept { return &meta_; }
  uint32_t size() const noexcept { return meta_.size; }
  void clear() noexcept;
  void draw(VideoFrame &frame, bool draw_text = true) const;
  void label_class_ids();
  void label_pet_person();

 private:
  TDLObject meta_{};
};

class FaceStore {
 public:
  void update(const FaceMeta &meta);
  FaceMeta snapshot() const;
  FaceMeta take();

 private:
  mutable std::mutex mutex_;
  FaceMeta latest_;
};

class ObjectStore {
 public:
  void update(const ObjectMeta &meta);
  ObjectMeta snapshot() const;
  ObjectMeta take();

 private:
  mutable std::mutex mutex_;
  ObjectMeta latest_;
};

using ObjectInferenceFunc = int (*)(TDLHandle, TDLModel, TDLImage, TDLObject *);

struct ObjectModelInfo {
  TDLModel model = TDL_MODEL_INVALID;
  ObjectInferenceFunc inference = nullptr;
};

ObjectModelInfo object_model_from_name(const std::string &name);
FaceMeta detect_faces(Handle &handle, TDLModel model, VideoFrame &frame);
ObjectMeta detect_objects(Handle &handle, TDLModel model, ObjectInferenceFunc inference,
                          VideoFrame &frame);
ObjectMeta detect_motion(Handle &handle, VideoFrame &frame, CVI_U8 threshold,
                         CVI_FLOAT min_area, CVI_U32 update_interval);

}  // namespace tdl
}  // namespace cvi_media
