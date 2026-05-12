# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
idf_build_get_property(target IDF_TARGET)

if(NOT target STREQUAL "esp32s3")
    # hap_master is S3-only (SPI master toward P4)
    idf_component_register()
    return()
endif()

idf_component_register(
    SRCS "hap_master.cpp"
    INCLUDE_DIRS "include"
    REQUIRES hap_protocol esp_driver_spi esp_driver_gpio freertos
)
