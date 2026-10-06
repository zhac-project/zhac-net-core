# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Host regression test for ws_server: TX serialisation, close-on-failed-send and logging outside
# the fd-table lock. Builds the real ws_server.cpp against the esp_http_server / FreeRTOS shims
# in stubs/ (real pthread mutexes, so two threads can genuinely race). Separate from any IDF
# unity app; build with plain cmake:
#   cmake -B build -S . && cmake --build build && ctest --test-dir build
cmake_minimum_required(VERSION 3.16)
project(ws_server_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(ws_server_host_test
    test_ws_tx.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ws_server.cpp)
target_include_directories(ws_server_host_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
target_link_libraries(ws_server_host_test PRIVATE Threads::Threads)
target_compile_options(ws_server_host_test PRIVATE -Wall -Wextra -Werror)

# ws_server queues httpd work (httpd_sess_trigger_close) from the httpd task itself. With
# CONFIG_HTTPD_QUEUE_WORK_BLOCKING a full control queue would block that task forever, so ws_server.cpp
# refuses to compile with it. Build the file with the option on and expect exactly that #error.
add_library(ws_server_blocking_probe OBJECT EXCLUDE_FROM_ALL ${CMAKE_CURRENT_SOURCE_DIR}/../../ws_server.cpp)
target_include_directories(ws_server_blocking_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
target_compile_definitions(ws_server_blocking_probe PRIVATE CONFIG_HTTPD_QUEUE_WORK_BLOCKING=1)

enable_testing()
add_test(NAME ws_server_host COMMAND ws_server_host_test)
add_test(NAME ws_server_rejects_blocking_queue_work
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target ws_server_blocking_probe)
set_tests_properties(ws_server_rejects_blocking_queue_work PROPERTIES
    PASS_REGULAR_EXPRESSION "needs CONFIG_HTTPD_QUEUE_WORK_BLOCKING=n")
