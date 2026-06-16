#define LOG_TAG "SampleOD"
#define LOG_LEVEL LOG_LEVEL_INFO

#include "middleware_utils.h"
#include "sample_utils.h"
#include "vi_vo_utils.h"

#include <core/utils/vpss_helper.h>
#include <cvi_comm.h>
#include <rtsp.h>
#include <sample_comm.h>
#include "tdl_sdk.h"
#include "tdl_v2_draw.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile bool bExit = false;

static TDLObject g_stObjMeta = {0};

MUTEXAUTOLOCK_INIT(ResultMutex);

/**
 * @brief Arguments for video encoder thread
 *
 */
typedef struct {
  SAMPLE_TDL_MW_CONTEXT *pstMWContext;
  TDLModel enOdModelId;
} SAMPLE_TDL_VENC_THREAD_ARG_S;

/**
 * @brief Arguments for ai thread
 *
 */
typedef struct {
  ODInferenceFunc inference_func;
  TDLModel enOdModelId;
  TDLHandle stTDLHandle;
} SAMPLE_TDL_TDL_THREAD_ARG_S;

void *run_venc(void *args) {
  printf("Enter encoder thread\n");
  SAMPLE_TDL_VENC_THREAD_ARG_S *pstArgs = (SAMPLE_TDL_VENC_THREAD_ARG_S *)args;
  VIDEO_FRAME_INFO_S stFrame;
  CVI_S32 s32Ret;
  TDLObject stObjMeta = {0};

  while (bExit == false) {
    s32Ret = CVI_VPSS_GetChnFrame(0, VPSS_CHN0, &stFrame, 2000);
    if (s32Ret != CVI_SUCCESS) {
      printf("CVI_VPSS_GetChnFrame chn0 failed with %#x\n", s32Ret);
      break;
    }

    {
      // Get detection result from global
      MutexAutoLock(ResultMutex, lock);
      TDL_CopyObjectMeta(&g_stObjMeta, &stObjMeta);
    }

    if (pstArgs->enOdModelId == TDL_MODEL_YOLOV8N_DET_PET_PERSON) {
      for (uint32_t oid = 0; oid < stObjMeta.size; oid++) {
        char name[256];
        if (stObjMeta.info[oid].class_id == 0) {
          sprintf(name, "cat: %.2f", stObjMeta.info[oid].score);
        }
        if (stObjMeta.info[oid].class_id == 1) {
          sprintf(name, "dog: %.2f", stObjMeta.info[oid].score);
        }
        if (stObjMeta.info[oid].class_id == 2) {
          sprintf(name, "person: %.2f", stObjMeta.info[oid].score);
        }
        memcpy(stObjMeta.info[oid].name, name, sizeof(stObjMeta.info[oid].name));
      }
    } else {
      for (uint32_t oid = 0; oid < stObjMeta.size; oid++) {
        char name[256];
        sprintf(name, "class:%d %.2f", stObjMeta.info[oid].class_id, stObjMeta.info[oid].score);
        memcpy(stObjMeta.info[oid].name, name, sizeof(stObjMeta.info[oid].name));
      }
    }

    s32Ret = SAMPLE_TDL_DrawObjectRect(&stObjMeta, &stFrame, true, SAMPLE_TDL_DefaultBrush());
    if (s32Ret != CVI_SUCCESS) {
      CVI_VPSS_ReleaseChnFrame(0, 0, &stFrame);
      printf("Draw fame fail!, ret=%x\n", s32Ret);
      goto error;
    }

    s32Ret = SAMPLE_TDL_Send_Frame_RTSP(&stFrame, pstArgs->pstMWContext);
  error:
    TDL_ReleaseObjectMeta(&stObjMeta);
    memset(&stObjMeta, 0, sizeof(TDLObject));
    CVI_VPSS_ReleaseChnFrame(0, 0, &stFrame);
    if (s32Ret != CVI_SUCCESS) {
      bExit = true;
    }
  }
  printf("Exit encoder thread\n");
  pthread_exit(NULL);
}

void *run_tdl_thread(void *args) {
  printf("Enter TDL thread\n");
  SAMPLE_TDL_TDL_THREAD_ARG_S *pstTDLArgs = (SAMPLE_TDL_TDL_THREAD_ARG_S *)args;
  VIDEO_FRAME_INFO_S stFrame;
  TDLObject stObjMeta = {0};

  CVI_S32 s32Ret;
  uint32_t counter = 0;
  while (bExit == false) {
    s32Ret = CVI_VPSS_GetChnFrame(0, VPSS_CHN1, &stFrame, 2000);

    if (s32Ret != CVI_SUCCESS) {
      printf("CVI_VPSS_GetChnFrame failed with %#x\n", s32Ret);
      goto get_frame_failed;
    }

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    TDLImage image = TDL_WrapFrame(&stFrame, false);
    if (image == NULL) {
      s32Ret = CVI_FAILURE;
      printf("TDL_WrapFrame failed\n");
      goto inf_error;
    }
    s32Ret = pstTDLArgs->inference_func(pstTDLArgs->stTDLHandle, pstTDLArgs->enOdModelId, image,
                                        &stObjMeta);
    TDL_DestroyImage(image);
    gettimeofday(&t1, NULL);

    if (s32Ret != CVI_SUCCESS) {
      printf("inference failed!, ret=%x\n", s32Ret);
      goto inf_error;
    }

    if (pstTDLArgs->enOdModelId == TDL_MODEL_YOLOV8N_DET_PET_PERSON) {
      uint32_t obj_id;
      for (uint32_t oid = 0; oid < stObjMeta.size; oid++) {
        obj_id = stObjMeta.info[oid].class_id;
        printf("%-7s %.2f %.2f %.2f %.2f %d %.2f\n",
                obj_id == 0 ? "cat:" : (obj_id == 1 ? "dog:" : (obj_id == 2 ? "person:" : "")),
                stObjMeta.info[oid].box.x1, stObjMeta.info[oid].box.y1,
                stObjMeta.info[oid].box.x2, stObjMeta.info[oid].box.y2,
                stObjMeta.info[oid].class_id, stObjMeta.info[oid].score);
      }
    } else {
      unsigned long execution_time = ((t1.tv_sec - t0.tv_sec) * 1000000 + t1.tv_usec - t0.tv_usec);
        if (counter++ % 5 == 0)
          printf("obj count: %d, take %.2f ms, width:%u\n", stObjMeta.size, (float)execution_time / 1000,
                 stFrame.stVFrame.u32Width);
    }

    {
      // Copy object detection results to global.
      MutexAutoLock(ResultMutex, lock);
      TDL_CopyObjectMeta(&stObjMeta, &g_stObjMeta);
    }

  inf_error:
    CVI_VPSS_ReleaseChnFrame(0, 1, &stFrame);
  get_frame_failed:
    TDL_ReleaseObjectMeta(&stObjMeta);
    memset(&stObjMeta, 0, sizeof(TDLObject));
    if (s32Ret != CVI_SUCCESS) {
      bExit = true;
    }
  }

  printf("Exit TDL thread\n");
  pthread_exit(NULL);
}

CVI_S32 get_middleware_config(SAMPLE_TDL_MW_CONFIG_S *pstMWConfig) {
  // Video Pipeline of this sample:
  //                                                       +------+
  //                                    CHN0 (VBPool 0)    | VENC |--------> RTSP
  //  +----+      +----------------+---------------------> +------+
  //  | VI |----->| VPSS 0 (DEV 1) |            +-----------------------+
  //  +----+      +----------------+----------> | VPSS 1 (DEV 0) TDL SDK |------------> AI model
  //                            CHN1 (VBPool 1) +-----------------------+  CHN0 (VBPool 2)

  // VI configuration
  //////////////////////////////////////////////////
  // Get VI configurations from ini file.
  CVI_S32 s32Ret = SAMPLE_TDL_Get_VI_Config(&pstMWConfig->stViConfig);
  if (s32Ret != CVI_SUCCESS || pstMWConfig->stViConfig.s32WorkingViNum <= 0) {
    printf("Failed to get senor infomation from ini file (/mnt/data/sensor_cfg.ini).\n");
    return -1;
  }

  // Get VI size
  PIC_SIZE_E enPicSize;
  s32Ret = SAMPLE_COMM_VI_GetSizeBySensor(pstMWConfig->stViConfig.astViInfo[0].stSnsInfo.enSnsType,
                                          &enPicSize);
  if (s32Ret != CVI_SUCCESS) {
    printf("Cannot get senor size\n");
    return s32Ret;
  }

  SIZE_S stSensorSize;
  s32Ret = SAMPLE_COMM_SYS_GetPicSize(enPicSize, &stSensorSize);
  if (s32Ret != CVI_SUCCESS) {
    printf("Cannot get senor size\n");
    return s32Ret;
  }

  // Setup frame size of video encoder to 1080p
  SIZE_S stVencSize = {
#ifdef CV180X
      .u32Width = 1280,
      .u32Height = 720,
#else
      .u32Width = 1920,
      .u32Height = 1080,
#endif
  };

  // VBPool configurations
  //////////////////////////////////////////////////
  pstMWConfig->stVBPoolConfig.u32VBPoolCount = 3;

  // VBPool 0 for VI
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[0].enFormat = VI_PIXEL_FORMAT;
#ifdef CV180X
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[0].u32BlkCount = 2;
#else
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[0].u32BlkCount = 3;
#endif
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[0].u32Height = stSensorSize.u32Height;
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[0].u32Width = stSensorSize.u32Width;
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[0].bBind = true;
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[0].u32VpssChnBinding = VPSS_CHN0;
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[0].u32VpssGrpBinding = (VPSS_GRP)0;

  // VBPool 1 for TDL frame
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[1].enFormat = VI_PIXEL_FORMAT;
#ifdef CV180X
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[1].u32BlkCount = 2;
#else
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[1].u32BlkCount = 3;
#endif
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[1].u32Height = stVencSize.u32Height;
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[1].u32Width = stVencSize.u32Width;
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[1].bBind = true;
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[1].u32VpssChnBinding = VPSS_CHN1;
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[1].u32VpssGrpBinding = (VPSS_GRP)0;

  // VBPool 2 for TDL preprocessing.
  // The input pixel format of TDL SDK models is eighter RGB 888 or RGB 888 Planar.
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[2].enFormat = PIXEL_FORMAT_RGB_888_PLANAR;
  // TDL SDK use only 1 buffer at the same time.
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[2].u32BlkCount = 1;
  // Considering the maximum input size of object detection model is 1024x768, we set same size
  // here.
#ifdef CV180X
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[2].u32Height = 256;
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[2].u32Width = 384;
#else
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[2].u32Height = 768;
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[2].u32Width = 1024;
#endif
  // Keep one extra pool for preprocessing buffers used by the v2 TDL SDK.
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[2].bBind = false;

  // VPSS configurations
  //////////////////////////////////////////////////

  // Create a VPSS Grp0 for main stream, video encoder, and TDL frame.
  pstMWConfig->stVPSSPoolConfig.u32VpssGrpCount = 1;
#ifndef CV186X
  pstMWConfig->stVPSSPoolConfig.stVpssMode.aenInput[0] = VPSS_INPUT_MEM;
  pstMWConfig->stVPSSPoolConfig.stVpssMode.enMode = VPSS_MODE_DUAL;
  pstMWConfig->stVPSSPoolConfig.stVpssMode.ViPipe[0] = 0;
  pstMWConfig->stVPSSPoolConfig.stVpssMode.aenInput[1] = VPSS_INPUT_ISP;
  pstMWConfig->stVPSSPoolConfig.stVpssMode.ViPipe[1] = 0;
#endif

  SAMPLE_TDL_VPSS_CONFIG_S *pstVpssConfig = &pstMWConfig->stVPSSPoolConfig.astVpssConfig[0];
  pstVpssConfig->bBindVI = true;

  // Assign device 1 to VPSS Grp0, because device1 has 3 outputs in dual mode.
  VPSS_GRP_DEFAULT_HELPER2(&pstVpssConfig->stVpssGrpAttr, stSensorSize.u32Width,
                           stSensorSize.u32Height, VI_PIXEL_FORMAT, 1);

  // Enable two channels for VENC and TDL frame
  pstVpssConfig->u32ChnCount = 2;

  // Bind VPSS Grp0 Ch0 with VI
  pstVpssConfig->u32ChnBindVI = VPSS_CHN0;
  VPSS_CHN_DEFAULT_HELPER(&pstVpssConfig->astVpssChnAttr[0], stVencSize.u32Width,
                          stVencSize.u32Height, VI_PIXEL_FORMAT, true);
  VPSS_CHN_DEFAULT_HELPER(&pstVpssConfig->astVpssChnAttr[1], stVencSize.u32Width,
                          stVencSize.u32Height, VI_PIXEL_FORMAT, true);

  // VENC
  //////////////////////////////////////////////////
  // Get default VENC configurations
  SAMPLE_TDL_Get_Input_Config(&pstMWConfig->stVencConfig.stChnInputCfg);
  pstMWConfig->stVencConfig.u32FrameWidth = stVencSize.u32Width;
  pstMWConfig->stVencConfig.u32FrameHeight = stVencSize.u32Height;

  // RTSP
  //////////////////////////////////////////////////
  // Get default RTSP configurations
  SAMPLE_TDL_Get_RTSP_Config(&pstMWConfig->stRTSPConfig.stRTSPConfig);

  return s32Ret;
}

static void SampleHandleSig(CVI_S32 signo) {
  signal(SIGINT, SIG_IGN);
  signal(SIGTERM, SIG_IGN);
  printf("handle signal, signo: %d\n", signo);
  if (SIGINT == signo || SIGTERM == signo) {
    bExit = true;
  }
}

int main(int argc, char *argv[]) {
  if (argc != 4 && argc != 3) {
    printf("sample_vi_od - Object detection sample\n\n");
    printf("Usage: %s MODEL_NAME MODEL_PATH [THRESHOLD]\n\n", argv[0]);
    printf("MODEL_NAME:\n"
           "\tmobiledetv2-person-vehicle  Person and vehicle detection (mobiledetv2)\n"
           "\tmobiledetv2-coco80          Coco 80 objects detection (mobiledetv2)\n"
           "\tmobiledetv2-vehicle         Vehicle detection (mobiledetv2)\n"
           "\tmobiledetv2-pedestrian      Pedestrian detection (mobiledetv2)\n"
           "\tyolov8-person-pets          Person, cat and dog detection (yolov8)\n"
           "\tyolov8n-det-person-vehicle  Person and vehicle detection (yolov8n)\n"
           "\tyolov8-coco80\n"
           "\tyolox\n\n");
    printf("MODEL_PATH: cvimodel path\n\n");
    printf("THRESHOLD: (optional) threshold for detection model (default: 0.5)\n\n");

    return -1;
  }

  signal(SIGINT, SampleHandleSig);
  signal(SIGTERM, SampleHandleSig);

  //  Step 1: Initialize middleware stuff.
  ////////////////////////////////////////////////////

  // Get middleware configurations including VI, VB, VPSS
  SAMPLE_TDL_MW_CONFIG_S stMWConfig = {0};
  CVI_S32 s32Ret = get_middleware_config(&stMWConfig);
  if (s32Ret != CVI_SUCCESS) {
    printf("get middleware configuration failed! ret=%x\n", s32Ret);
    return -1;
  }

  // Initialize middleware.
  SAMPLE_TDL_MW_CONTEXT stMWContext = {0};
  s32Ret = SAMPLE_TDL_Init_WM(&stMWConfig, &stMWContext);
  if (s32Ret != CVI_SUCCESS) {
    printf("init middleware failed! ret=%x\n", s32Ret);
    return -1;
  }

  // Step 2: Create and setup TDL SDK
  ///////////////////////////////////////////////////

  TDLHandle stTDLHandle = TDL_CreateHandle(0);
  if (stTDLHandle == NULL) {
    s32Ret = CVI_FAILURE;
    goto create_tdl_fail;
  }
  // Step 3: Open and setup TDL models
  ///////////////////////////////////////////////////

  // Get inference function pointer and model id of object deteciton according to model name.
  ODInferenceFunc inference_func;
  TDLModel enOdModelId = TDL_MODEL_INVALID;
  if (get_od_model_info(argv[1], &enOdModelId, &inference_func) != CVI_SUCCESS) {
    printf("unsupported model: %s\n", argv[1]);
    s32Ret = CVI_FAILURE;
    goto setup_tdl_fail;
  }

  GOTO_IF_FAILED(TDL_OpenModel(stTDLHandle, enOdModelId, argv[2], NULL), s32Ret, setup_tdl_fail);

  if (argc == 4) {
    float fThreshold = atof(argv[3]);
    if (fThreshold < 0.0 || fThreshold > 1.0) {
      printf("wrong threshold value: %f\n", fThreshold);
      s32Ret = CVI_FAILURE;
      goto setup_tdl_fail;
    } else {
      printf("set threshold to %f\n", fThreshold);
    }
    GOTO_IF_FAILED(TDL_SetModelThreshold(stTDLHandle, enOdModelId, fThreshold), s32Ret,
                   setup_tdl_fail);
  }

  // Step 4: Run models in thread.
  ///////////////////////////////////////////////////

  pthread_t stVencThread, stTDLThread;
  SAMPLE_TDL_VENC_THREAD_ARG_S venc_args = {
      .pstMWContext = &stMWContext,
      .enOdModelId = enOdModelId,
  };

  SAMPLE_TDL_TDL_THREAD_ARG_S ai_args = {
      .enOdModelId = enOdModelId,
      .inference_func = inference_func,
      .stTDLHandle = stTDLHandle,
  };

  pthread_create(&stVencThread, NULL, run_venc, &venc_args);
  pthread_create(&stTDLThread, NULL, run_tdl_thread, &ai_args);

  // Thread for video encoder
  pthread_join(stVencThread, NULL);

  // Thread for TDL inference
  pthread_join(stTDLThread, NULL);

setup_tdl_fail:
  TDL_CloseModel(stTDLHandle, enOdModelId);
  TDL_DestroyHandle(stTDLHandle);
create_tdl_fail:
  SAMPLE_TDL_Destroy_MW(&stMWContext);

  return 0;
}
