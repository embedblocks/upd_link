#pragma once
#include "freertos/FreeRTOS.h"
static inline TickType_t xTaskGetTickCount(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t us = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
    return (TickType_t)(us * configTICK_RATE_HZ / 1000000u);
}
