# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
idf_component_register(
    SRCS "main.cpp" "groups_store.cpp"
         "api_system.cpp"
         "api_devices.cpp"
         "api_rules.cpp"
         "api_scripts.cpp"
         "api_groups.cpp"
         "hap_bridge.cpp"
         "log_ring.cpp"
         "rest_devices.cpp"
         "rest_rules.cpp"
         "rest_ops.cpp"
         "ws_bridge.cpp"
         "wifi_mgr.cpp"
         "metrics_mqtt.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        hap_protocol
        zap_common
        hap_master
        hap_session
        hap_json
        ws_server
        mqtt_gw
        tg_gw
        metrics
        mbedtls
        arduinojson
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        esp_timer
        esp_https_ota
        app_update
        esp_http_client
        freertos
        spiffs
        driver
)

spiffs_create_partition_image(spiffs ../www-spa/dist FLASH_IN_PROJECT)
