// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "hap_protocol.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <functional>

using HapFrameCallback = std::function<void(const HapFrame&)>;

void hap_master_init();
void hap_master_set_task_handle(TaskHandle_t h);
void hap_master_send(const HapFrame& frame);
void hap_master_recv();
void hap_master_set_callback(HapFrameCallback cb);
