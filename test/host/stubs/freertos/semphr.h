// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "FreeRTOS.h"
typedef void* SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)          { static int m; return &m; }
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { static int m; return &m; }
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t)          { return pdTRUE; }
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t)                      { return pdTRUE; }
static inline BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t, TickType_t){ return pdTRUE; }
static inline BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t)            { return pdTRUE; }
static inline void       vSemaphoreDelete(SemaphoreHandle_t)                    {}
