// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RainMaker uplink bridge — claim/connect state machine + esp-rainmaker SDK
// wiring (Task 13 of the RainMaker Bridge plan). Builds on the Task 11
// skeleton: the lifecycle surface (rainmaker_gw.h) is unchanged apart from
// the new rainmaker_gw_assoc_start() entry point; this translation unit now
// actually drives s_state instead of leaving it pinned at RMK_ST_DISABLED.
//
// The node-init / device-create / start / user-node-mapping call sequence
// below was proven on real hardware by the P0 spike (all four gates PASS —
// claim, connect, associate, cloud->node write). See this repo's
// spike/rainmaker-p0 history for the original throwaway harness; this file
// is the production adaptation: the mapping credentials are now a caller-
// supplied argument (rainmaker_gw_assoc_start) instead of a baked pair, and
// the whole thing lives behind the component's public API rather than
// inline in app_main().
//
// Compiled as C++ despite the .c name/extension — see the LANGUAGE CXX
// override + comment in this component's CMakeLists.txt for why (short
// version: zap_store.h's include chain uses C++-only syntax, and
// zhac_uplink_get() only has C++-mangled linkage, so a real C translation
// unit can neither #include zap_store.h nor link against it).
// rainmaker_gw.h's own `extern "C"` guard keeps this file's PUBLIC API
// (rainmaker_gw_init/_state/_node_id/_assoc_start) at plain C linkage
// regardless, so nothing about the component's external contract changes.
#include "sdkconfig.h"
#include "rainmaker_gw.h"

#include "esp_log.h"

#if CONFIG_ZHAC_RAINMAKER_ENABLE
#include <cstring>

#include "zap_store.h"

#include "esp_event.h"
#include "esp_rmaker_core.h"
#include "esp_rmaker_common_events.h"
#include "esp_rmaker_user_mapping.h"
#include "esp_rmaker_standard_devices.h"

// Never included/called from this file: esp_rmaker_system_serv_config (or
// any other System service), esp_rmaker_ota_*, esp_rmaker_schedule_*,
// esp_rmaker_scenes_*, esp_rmaker_local_ctrl_* — spec section 6: the cloud
// must not be able to reboot, factory-reset, OTA, or otherwise reach into
// ZHAC. The only device this bridge ever creates is the one hardcoded
// Switch below.

static const char* TAG = "rainmaker_gw";
static char s_node_id[32] = "";
#endif  // CONFIG_ZHAC_RAINMAKER_ENABLE

static rmk_state_t s_state = RMK_ST_DISABLED;

#if CONFIG_ZHAC_RAINMAKER_ENABLE

// Cloud->node param write for the one hardcoded test device: log it, then
// ack it straight back so the phone app's toggle reflects immediately
// (ZHAC-Test has no real backing hardware — Task 14's HW gate just needs
// something to flip). Matches the P0 spike's write-cb byte-for-byte.
static esp_err_t rmk_write_cb(const esp_rmaker_device_t* device,
                               const esp_rmaker_param_t* param,
                               const esp_rmaker_param_val_t val,
                               void* priv_data,
                               esp_rmaker_write_ctx_t* ctx) {
    (void)priv_data;
    (void)ctx;
    ESP_LOGI(TAG, "WRITE-CB device=%s param=%s val=%s",
              esp_rmaker_device_get_name(device),
              esp_rmaker_param_get_name(param),
              val.type == RMAKER_VAL_TYPE_BOOLEAN ? (val.val.b ? "true" : "false") : "(non-bool)");
    esp_rmaker_param_update_and_report((esp_rmaker_param_t*)param, val);
    return ESP_OK;
}

// Single handler for both RainMaker event bases; base+id together select
// the transition. Event names below are pulled straight from the resolved
// SDK headers (esp_rmaker_core.h / esp_rmaker_common_events.h) — the
// brief's Step-2 sketch used the same names and they compiled as-is.
static void rmk_event_handler(void* arg, esp_event_base_t base, int32_t id, void* event_data) {
    (void)arg;
    (void)event_data;

    if (base == RMAKER_EVENT) {
        switch (id) {
        case RMAKER_EVENT_CLAIM_SUCCESSFUL:
            ESP_LOGI(TAG, "claim successful — connecting");
            s_state = RMK_ST_CONNECTING;
            break;
        case RMAKER_EVENT_CLAIM_FAILED:
            ESP_LOGE(TAG, "claim FAILED — needs operator action");
            s_state = RMK_ST_CLAIM_FAILED;
            break;
        case RMAKER_EVENT_USER_NODE_MAPPING_DONE:
            ESP_LOGI(TAG, "user-node mapping done — bridge READY");
            s_state = RMK_ST_READY;
            break;
        case RMAKER_EVENT_USER_NODE_MAPPING_RESET:
            ESP_LOGW(TAG, "user-node mapping reset — bridge UNASSOCIATED");
            s_state = RMK_ST_UNASSOCIATED;
            break;
        default:
            break;
        }
        return;
    }

    if (base == RMAKER_COMMON_EVENT) {
        switch (id) {
        case RMAKER_MQTT_EVENT_CONNECTED:
            if (s_state == RMK_ST_CONNECTING || s_state == RMK_ST_BACKOFF) {
                // esp_rmaker_core.c's esp_rmaker_post_mqtt_connect_task()
                // only posts RMAKER_EVENT_USER_NODE_MAPPING_DONE the FIRST
                // time a mapping completes. On every later MQTT connect —
                // a reboot with a mapping already persisted, or simply
                // recovering from BACKOFF — the SDK goes straight to
                // params-mqtt-init without re-posting that event. Landing
                // unconditionally on UNASSOCIATED here would strand an
                // already-associated node there forever after any
                // transient reconnect, so check the persisted mapping
                // state and skip straight to READY when it is already
                // done (verified against esp_rmaker_core.c v1.16.0).
                if (esp_rmaker_user_node_mapping_get_state() == ESP_RMAKER_USER_MAPPING_DONE) {
                    ESP_LOGI(TAG, "MQTT connected, mapping already done — READY");
                    s_state = RMK_ST_READY;
                } else {
                    ESP_LOGI(TAG, "MQTT connected — UNASSOCIATED, awaiting user-node mapping");
                    s_state = RMK_ST_UNASSOCIATED;
                }
            }
            break;
        case RMAKER_MQTT_EVENT_DISCONNECTED:
            // The SDK's MQTT client auto-reconnects on its own; BACKOFF
            // here only tracks that fact for rainmaker_gw_state() callers.
            // components/remote_client/include/remote_backoff.h's
            // exponential-plus-jitter shape was read per the brief's Step
            // 1, but a second, locally-driven retry timer would just race
            // the SDK's own internal backoff rather than help — so it is
            // deliberately not reimplemented here.
            if (s_state == RMK_ST_READY) {
                ESP_LOGW(TAG, "MQTT disconnected — BACKOFF (SDK auto-reconnects)");
                s_state = RMK_ST_BACKOFF;
            }
            break;
        default:
            break;
        }
    }
}

#endif  // CONFIG_ZHAC_RAINMAKER_ENABLE

void rainmaker_gw_init(void) {
#if CONFIG_ZHAC_RAINMAKER_ENABLE
    if (zhac_uplink_get() != ZHAC_UPLINK_RAINMAKER) {
        ESP_LOGI(TAG, "ZHAC_RAINMAKER_ENABLE=y but uplink selector != "
                      "RAINMAKER — bridge stays inactive");
        return;
    }
    ESP_LOGI(TAG, "RainMaker uplink selected — starting claim/connect state machine");

    esp_err_t herr1 = esp_event_handler_register(RMAKER_EVENT, ESP_EVENT_ANY_ID, &rmk_event_handler, NULL);
    esp_err_t herr2 = esp_event_handler_register(RMAKER_COMMON_EVENT, ESP_EVENT_ANY_ID, &rmk_event_handler, NULL);
    if (herr1 != ESP_OK || herr2 != ESP_OK) {
        ESP_LOGW(TAG, "event handler registration incomplete (rmaker=%s common=%s) — "
                      "state tracking may be inaccurate",
                 esp_err_to_name(herr1), esp_err_to_name(herr2));
    }

    // P0 Gate-1 finding: enable_time_sync=true boot-loops the device (an
    // assert inside sntp_setoperatingmode) because ZHAC already runs its
    // own SNTP client from main.cpp. RainMaker must not touch SNTP.
    esp_rmaker_config_t rcfg = { .enable_time_sync = false };
    esp_rmaker_node_t* node = esp_rmaker_node_init(&rcfg, CONFIG_ZHAC_RAINMAKER_NODE_NAME, "Zigbee Gateway");
    if (!node) {
        ESP_LOGE(TAG, "esp_rmaker_node_init failed");
        s_state = RMK_ST_CLAIM_FAILED;
        return;
    }

    // ONE hardcoded Switch so the phone app has something to toggle at the
    // Task-14 hardware gate; no real backing hardware behind it.
    esp_rmaker_device_t* sw = esp_rmaker_switch_device_create("ZHAC-Test", NULL, false);
    if (sw) {
        esp_rmaker_device_add_cb(sw, rmk_write_cb, NULL);
        esp_rmaker_node_add_device(node, sw);
        ESP_LOGI(TAG, "ZHAC-Test switch device added");
    } else {
        ESP_LOGE(TAG, "switch device create failed — node will still claim/connect without it");
    }

    esp_err_t serr = esp_rmaker_start();
    if (serr != ESP_OK) {
        ESP_LOGE(TAG, "esp_rmaker_start failed: %s", esp_err_to_name(serr));
        s_state = RMK_ST_CLAIM_FAILED;
        return;
    }
    s_state = RMK_ST_INIT_CLAIM;

    const char* nid = esp_rmaker_get_node_id();
    if (nid) {
        strlcpy(s_node_id, nid, sizeof(s_node_id));
        ESP_LOGI(TAG, "node id: %s", s_node_id);
    } else {
        ESP_LOGW(TAG, "node id not available yet");
    }
#endif  // CONFIG_ZHAC_RAINMAKER_ENABLE
    // With the flag off (or flag on but uplink != RAINMAKER, handled by the
    // early return above) s_state stays RMK_ST_DISABLED.
}

rmk_state_t rainmaker_gw_state(void) {
    return s_state;
}

const char* rainmaker_gw_node_id(void) {
#if CONFIG_ZHAC_RAINMAKER_ENABLE
    return s_node_id;
#else
    return "";
#endif
}

esp_err_t rainmaker_gw_assoc_start(const char* user_id, const char* secret) {
#if CONFIG_ZHAC_RAINMAKER_ENABLE
    // DISABLED: flag off / uplink != RainMaker / init short-circuited.
    // CLAIM_FAILED: agent started but has no usable cloud identity.
    // Neither has a live RainMaker agent to hand the mapping call to.
    if (s_state == RMK_ST_DISABLED || s_state == RMK_ST_CLAIM_FAILED) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_rmaker_start_user_node_mapping((char*)user_id, (char*)secret);
#else
    (void)user_id;
    (void)secret;
    return ESP_ERR_INVALID_STATE;
#endif
}
