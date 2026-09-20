// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>

#include "ArduinoJson.h"

// Home Assistant discovery on the dual-chip S3 (see zhac-components ha_bridge).
void ha_glue_start();
// The per-attribute fan-out of one BULK_STATE_UPDATE's "attrs" object.
void ha_glue_publish_attrs(uint64_t ieee, JsonObjectConst attrs);
