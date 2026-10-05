/* Host stub: FreeRTOS types, ticks derived from CLOCK_MONOTONIC at configTICK_RATE_HZ. */
#pragma once
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#ifndef configTICK_RATE_HZ
#define configTICK_RATE_HZ 1000
#endif
#define portTICK_PERIOD_MS ((uint32_t)(1000 / configTICK_RATE_HZ))
#define portMAX_DELAY      UINT32_MAX
#define pdTRUE  1
#define pdFALSE 0
typedef uint32_t TickType_t;
typedef int      BaseType_t;
