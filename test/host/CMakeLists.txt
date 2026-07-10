# SPDX-License-Identifier: AGPL-3.0-or-later
# Host tests for net-core app-layer serialization safety (CODEX H-01/H-02).
# Compiles the real main/ sources against minimal ESP/NVS/FreeRTOS stubs under
# ASan+UBSan.
cmake_minimum_required(VERSION 3.16)
project(net_core_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -g -O1
    -fsanitize=address,undefined -fno-omit-frame-pointer)
add_link_options(-fsanitize=address,undefined)

set(MAIN ${CMAKE_CURRENT_SOURCE_DIR}/../../main)

add_executable(json_safety_host
    test_json_safety.cpp
    ${MAIN}/groups_store.cpp)
target_include_directories(json_safety_host PRIVATE
    ${MAIN}
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs)

enable_testing()
add_test(NAME json_safety COMMAND json_safety_host)
