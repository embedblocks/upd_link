#pragma once
#include "freertos/FreeRTOS.h"
typedef pthread_mutex_t  StaticSemaphore_t;
typedef pthread_mutex_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s) { pthread_mutex_init(s, NULL); return s; }
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t t) { return (t == 0 ? pthread_mutex_trylock(m) : pthread_mutex_lock(m)) == 0; }
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t m) { return pthread_mutex_unlock(m) == 0; }
static inline void vSemaphoreDelete(SemaphoreHandle_t m) { pthread_mutex_destroy(m); }
