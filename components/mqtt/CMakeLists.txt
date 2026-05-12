set(ESP_MQTT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/esp-mqtt")

set(srcs
    "${ESP_MQTT_DIR}/mqtt_client.c"
    "${ESP_MQTT_DIR}/lib/mqtt_msg.c"
    "${ESP_MQTT_DIR}/lib/mqtt_outbox.c"
    "${ESP_MQTT_DIR}/lib/platform_esp32_idf.c"
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "${ESP_MQTT_DIR}/include"
    PRIV_INCLUDE_DIRS "${ESP_MQTT_DIR}/lib/include"
    REQUIRES esp_event tcp_transport
    PRIV_REQUIRES esp_timer http_parser esp_hw_support heap
)
