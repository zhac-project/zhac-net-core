// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RainMaker uplink bridge — skeleton (Task 11 of the RainMaker Bridge plan).
// No esp_rmaker_* calls yet; the claim/connect state machine and the actual
// esp_rmaker_node_init() wiring land in Task 13. This translation unit only
// establishes the lifecycle surface declared in rainmaker_gw.h (state var +
// the three entry points) and, when CONFIG_ZHAC_RAINMAKER_ENABLE is on,
// checks whether the runtime uplink selector actually asks for RainMaker so
// the log makes the two "off" reasons (build flag vs. runtime selector)
// distinguishable from the console.
//
// Compiled as C++ despite the .c name/extension — see the LANGUAGE CXX
// override + comment in this component's CMakeLists.txt for why (short
// version: zap_store.h's include chain uses C++-only syntax, and
// zhac_uplink_get() only has C++-mangled linkage, so a real C translation
// unit can neither #include zap_store.h nor link against it).
// rainmaker_gw.h's own `extern "C"` guard keeps this file's PUBLIC API
// (rainmaker_gw_init/_state/_node_id) at plain C linkage regardless, so
// nothing about the component's external contract changes.
#include "sdkconfig.h"
#include "rainmaker_gw.h"

#include "esp_log.h"

#if CONFIG_ZHAC_RAINMAKER_ENABLE
#include "zap_store.h"
#endif

static rmk_state_t s_state = RMK_ST_DISABLED;

void rainmaker_gw_init(void) {
#if CONFIG_ZHAC_RAINMAKER_ENABLE
    // Scoped to this #if — with the flag off there is nothing to log, and
    // an unconditional file-scope TAG would warn unused in that build.
    static const char* TAG = "rainmaker_gw";
    if (zhac_uplink_get() == ZHAC_UPLINK_RAINMAKER) {
        ESP_LOGI(TAG, "RainMaker uplink selected — bridge skeleton present; "
                      "claim/connect state machine lands in Task 13");
    } else {
        ESP_LOGI(TAG, "ZHAC_RAINMAKER_ENABLE=y but uplink selector != "
                      "RAINMAKER — bridge stays inactive");
    }
#endif
    // No esp_rmaker_* calls and no state transitions yet (Task 13 owns the
    // claim/connect state machine). s_state stays RMK_ST_DISABLED here in
    // both Kconfig states and regardless of the uplink selector.
}

rmk_state_t rainmaker_gw_state(void) {
    return s_state;
}

const char* rainmaker_gw_node_id(void) {
    // No claim logic yet (Task 13) — always unclaimed.
    return "";
}
