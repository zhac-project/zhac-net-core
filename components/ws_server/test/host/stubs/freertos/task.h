// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "FreeRTOS.h"
typedef void* TaskHandle_t;
inline TaskHandle_t xTaskGetCurrentTaskHandle() { return reinterpret_cast<TaskHandle_t>(pthread_self()); }
