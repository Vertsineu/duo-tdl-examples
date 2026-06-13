#define LOG_TAG "SampleVIRtsp"
#define LOG_LEVEL LOG_LEVEL_INFO

#include "middleware_utils.h"

#include <core/utils/vpss_helper.h>
#include <cvi_comm.h>
#include <sample_comm.h>

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile bool bExit = false;

typedef struct {
  SAMPLE_TDL_MW_CONTEXT *pstMWContext;
} SAMPLE_VI_RTSP_THREAD_ARG_S;

static void SampleHandleSig(CVI_S32 signo) {
  signal(SIGINT, SIG_IGN);
  signal(SIGTERM, SIG_IGN);
  printf("handle signal, signo: %d\n", signo);
  if (SIGINT == signo || SIGTERM == signo) {
    bExit = true;
  }
}

static CVI_S32 parse_u32(const char *text, CVI_U32 *value) {
  char *end = NULL;
  long parsed = strtol(text, &end, 10);
  if (end == text || *end != '\0' || parsed <= 0) {
    return CVI_FAILURE;
  }
  *value = (CVI_U32)parsed;
  return CVI_SUCCESS;
}

static void *run_rtsp(void *args) {
  printf("Enter RTSP thread\n");

  SAMPLE_VI_RTSP_THREAD_ARG_S *pstArgs = (SAMPLE_VI_RTSP_THREAD_ARG_S *)args;
  VIDEO_FRAME_INFO_S stFrame;

  while (bExit == false) {
    CVI_S32 s32Ret = CVI_VPSS_GetChnFrame(0, VPSS_CHN0, &stFrame, 2000);
    if (s32Ret != CVI_SUCCESS) {
      printf("CVI_VPSS_GetChnFrame chn0 failed with %#x\n", s32Ret);
      bExit = true;
      break;
    }

    s32Ret = SAMPLE_TDL_Send_Frame_RTSP(&stFrame, pstArgs->pstMWContext);
    CVI_VPSS_ReleaseChnFrame(0, VPSS_CHN0, &stFrame);

    if (s32Ret != CVI_SUCCESS) {
      printf("Send RTSP frame failed, ret=%#x\n", s32Ret);
      bExit = true;
      break;
    }
  }

  printf("Exit RTSP thread\n");
  pthread_exit(NULL);
}

static CVI_S32 get_middleware_config(SAMPLE_TDL_MW_CONFIG_S *pstMWConfig, SIZE_S stVencSize,
                                     const char *codec) {
  CVI_S32 s32Ret = SAMPLE_TDL_Get_VI_Config(&pstMWConfig->stViConfig);
  if (s32Ret != CVI_SUCCESS || pstMWConfig->stViConfig.s32WorkingViNum <= 0) {
    printf("Failed to get sensor information from ini file (/mnt/data/sensor_cfg.ini).\n");
    return CVI_FAILURE;
  }

  PIC_SIZE_E enPicSize;
  s32Ret = SAMPLE_COMM_VI_GetSizeBySensor(pstMWConfig->stViConfig.astViInfo[0].stSnsInfo.enSnsType,
                                          &enPicSize);
  if (s32Ret != CVI_SUCCESS) {
    printf("Cannot get sensor size\n");
    return s32Ret;
  }

  SIZE_S stSensorSize;
  s32Ret = SAMPLE_COMM_SYS_GetPicSize(enPicSize, &stSensorSize);
  if (s32Ret != CVI_SUCCESS) {
    printf("Cannot get sensor size\n");
    return s32Ret;
  }

  pstMWConfig->stVBPoolConfig.u32VBPoolCount = 1;
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[0].enFormat = VI_PIXEL_FORMAT;
#ifdef CV180X
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[0].u32BlkCount = 2;
#else
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[0].u32BlkCount = 5;
#endif
  pstMWConfig->stVBPoolConfig.astVBPoolSetup[0].u32Height = stVencSize.u32Height;
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

  SAMPLE_TDL_VPSS_CONFIG_S *pstVpssConfig = &pstMWConfig->stVPSSPoolConfig.astVpssConfig[0];
  pstVpssConfig->bBindVI = true;
  pstVpssConfig->u32ChnCount = 1;
  pstVpssConfig->u32ChnBindVI = 0;
  VPSS_GRP_DEFAULT_HELPER2(&pstVpssConfig->stVpssGrpAttr, stSensorSize.u32Width,
                           stSensorSize.u32Height, VI_PIXEL_FORMAT, 1);
  VPSS_CHN_DEFAULT_HELPER(&pstVpssConfig->astVpssChnAttr[0], stVencSize.u32Width,
                          stVencSize.u32Height, VI_PIXEL_FORMAT, true);

  SAMPLE_TDL_Get_Input_Config(&pstMWConfig->stVencConfig.stChnInputCfg);
  strncpy(pstMWConfig->stVencConfig.stChnInputCfg.codec, codec,
          sizeof(pstMWConfig->stVencConfig.stChnInputCfg.codec) - 1);
  pstMWConfig->stVencConfig.u32FrameWidth = stVencSize.u32Width;
  pstMWConfig->stVencConfig.u32FrameHeight = stVencSize.u32Height;

  SAMPLE_TDL_Get_RTSP_Config(&pstMWConfig->stRTSPConfig.stRTSPConfig);
  return CVI_SUCCESS;
}

int main(int argc, char *argv[]) {
  SIZE_S stVencSize = {
      .u32Width = 1280,
      .u32Height = 720,
  };
  const char *codec = "h264";

  if (argc != 1 && argc != 3 && argc != 4) {
    printf("\nUsage: %s [WIDTH HEIGHT [h264|h265]]\n\n", argv[0]);
    printf("Default: %s 1280 720 h264\n", argv[0]);
    printf("RTSP URL: rtsp://<duo-ip>/%s\n\n", codec);
    return CVI_FAILURE;
  }

  if (argc >= 3) {
    if (parse_u32(argv[1], &stVencSize.u32Width) != CVI_SUCCESS ||
        parse_u32(argv[2], &stVencSize.u32Height) != CVI_SUCCESS) {
      printf("Invalid width or height.\n");
      return CVI_FAILURE;
    }
  }

  if (argc == 4) {
    if (strcmp(argv[3], "h264") != 0 && strcmp(argv[3], "h265") != 0) {
      printf("Unsupported codec: %s. Use h264 or h265.\n", argv[3]);
      return CVI_FAILURE;
    }
    codec = argv[3];
  }

  if (SAMPLE_TDL_Get_PIC_Size(stVencSize.u32Width, stVencSize.u32Height) == PIC_BUTT) {
    printf("Unsupported frame size: %ux%u. Try 1280x720 or 1920x1080.\n", stVencSize.u32Width,
           stVencSize.u32Height);
    return CVI_FAILURE;
  }

  signal(SIGINT, SampleHandleSig);
  signal(SIGTERM, SampleHandleSig);

  SAMPLE_TDL_MW_CONFIG_S stMWConfig = {0};
  CVI_S32 s32Ret = get_middleware_config(&stMWConfig, stVencSize, codec);
  if (s32Ret != CVI_SUCCESS) {
    printf("get middleware configuration failed! ret=%#x\n", s32Ret);
    return CVI_FAILURE;
  }

  SAMPLE_TDL_MW_CONTEXT stMWContext = {0};
  s32Ret = SAMPLE_TDL_Init_WM(&stMWConfig, &stMWContext);
  if (s32Ret != CVI_SUCCESS) {
    printf("init middleware failed! ret=%#x\n", s32Ret);
    return CVI_FAILURE;
  }

  printf("RTSP stream is ready: rtsp://<duo-ip>/%s\n", codec);

  pthread_t stRtspThread;
  SAMPLE_VI_RTSP_THREAD_ARG_S args = {
      .pstMWContext = &stMWContext,
  };

  pthread_create(&stRtspThread, NULL, run_rtsp, &args);
  pthread_join(stRtspThread, NULL);

  SAMPLE_TDL_Destroy_MW(&stMWContext);
  return CVI_SUCCESS;
}
