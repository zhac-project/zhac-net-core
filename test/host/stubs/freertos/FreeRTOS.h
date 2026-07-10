// SPDX-License-Identifier: AGPL-3.0-or-later
// Minimal host stub — net-core app-layer JSON safety tests.
#pragma once
#include <cstdint>
typedef int          BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t     TickType_t;
#define pdTRUE  1
#define pdFALSE 0
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(x) (x)
