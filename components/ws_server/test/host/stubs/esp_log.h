// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
// ESP_LOG* forwards to host_log(), supplied by the test (it watches for the failure warnings);
// the arguments are still evaluated, so there is no "set but unused" under -Werror.
void host_log(const char* tag, const char* fmt, ...);
#define ESP_LOGE(tag, ...) host_log(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) host_log(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) host_log(tag, __VA_ARGS__)
