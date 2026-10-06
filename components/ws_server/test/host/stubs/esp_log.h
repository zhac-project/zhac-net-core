// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
// Logging is a no-op, but the arguments are still evaluated (no "set but unused" under -Werror).
static inline void host_log(const char*, ...) {}
#define ESP_LOGE(tag, ...) host_log(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) host_log(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) host_log(tag, __VA_ARGS__)
