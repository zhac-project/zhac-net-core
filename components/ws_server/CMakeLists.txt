# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
idf_component_register(
    SRCS "ws_server.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server freertos
)
