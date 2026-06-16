#include "sample_utils.h"

#include <string.h>

CVI_S32 get_od_model_info(const char *model_name, TDLModel *model_index,
                          ODInferenceFunc *inference_func) {
  CVI_S32 ret = CVI_SUCCESS;
  *inference_func = TDL_Detection;

  if (strcmp(model_name, "mbv2-det-person") == 0 ||
      strcmp(model_name, "mobiledetv2-pedestrian") == 0) {
    *model_index = TDL_MODEL_MBV2_DET_PERSON;
  } else if (strcmp(model_name, "yolov8-person-pets") == 0 ||
             strcmp(model_name, "yolov8n-det-pet-person") == 0 ||
             strcmp(model_name, "yolov8n_det_pet_person") == 0) {
    *model_index = TDL_MODEL_YOLOV8N_DET_PET_PERSON;
  } else if (strcmp(model_name, "yolov8n-det-person-vehicle") == 0 ||
             strcmp(model_name, "yolov8n_det_person_vehicle") == 0 ||
             strcmp(model_name, "mobiledetv2-person-vehicle") == 0 ||
             strcmp(model_name, "mobiledetv2-vehicle") == 0) {
    *model_index = TDL_MODEL_YOLOV8N_DET_PERSON_VEHICLE;
  } else if (strcmp(model_name, "yolov8-coco80") == 0 ||
             strcmp(model_name, "mobiledetv2-coco80") == 0) {
    *model_index = TDL_MODEL_YOLOV8_DET_COCO80;
  } else if (strcmp(model_name, "yolox") == 0 ||
             strcmp(model_name, "yolox-coco80") == 0) {
    *model_index = TDL_MODEL_YOLOX_DET_COCO80;
  } else {
    ret = CVI_FAILURE;
  }

  return ret;
}
