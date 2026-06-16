#ifndef SAMPLE_UTILS_H_
#define SAMPLE_UTILS_H_

#include <pthread.h>
#include "cvi_comm.h"
#include "tdl_sdk.h"

#define RETURN_IF_FAILED(func)    \
  do {                            \
    CVI_S32 tdl_ret = (func);     \
    if (tdl_ret != CVI_SUCCESS) { \
      goto tdl_failed;            \
    }                             \
  } while (0)

#define GOTO_IF_FAILED(func, result, label)                              \
  do {                                                                   \
    result = (func);                                                     \
    if (result != CVI_SUCCESS) {                                         \
      printf("failed! ret=%#x, at %s:%d\n", result, __FILE__, __LINE__); \
      goto label;                                                        \
    }                                                                    \
  } while (0)

typedef int (*ODInferenceFunc)(TDLHandle, TDLModel, TDLImage, TDLObject *);

CVI_S32 get_od_model_info(const char *model_name, TDLModel *model_id,
                          ODInferenceFunc *inference_func);

#define MUTEXAUTOLOCK_INIT(mutex) pthread_mutex_t AUTOLOCK_##mutex = PTHREAD_MUTEX_INITIALIZER;

#define MutexAutoLock(mutex, lock)                                                \
  __attribute__((cleanup(AutoUnLock))) pthread_mutex_t *lock = &AUTOLOCK_##mutex; \
  pthread_mutex_lock(lock);

__attribute__((always_inline)) inline void AutoUnLock(void *mutex) {
  pthread_mutex_unlock(*(pthread_mutex_t **)mutex);
}

#endif
