# SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
project(remote_client_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)

enable_testing()

# Backoff test uses only the pure header and cpp
add_executable(remote_backoff_tests
    remote_backoff_test.cpp
    ../remote_backoff.cpp
)
target_include_directories(remote_backoff_tests PRIVATE ../include)
add_test(NAME remote_backoff COMMAND remote_backoff_tests)

# State machine test
add_executable(remote_state_tests
    remote_state_test.cpp
    ../remote_state.cpp
)
target_include_directories(remote_state_tests PRIVATE ../include)
add_test(NAME remote_state COMMAND remote_state_tests)

# Allow-list test
add_executable(remote_allow_tests
    remote_allow_test.cpp
    ../remote_allow.cpp
)
target_include_directories(remote_allow_tests PRIVATE ../include)
add_test(NAME remote_allow COMMAND remote_allow_tests)

# NVS layer test — stub provides in-memory kv_* impl
add_executable(remote_nvs_tests
    remote_nvs_test.cpp
    remote_nvs_stub.cpp
    ../remote_nvs.cpp
)
target_include_directories(remote_nvs_tests PRIVATE ../include)
target_compile_definitions(remote_nvs_tests PRIVATE REMOTE_NVS_HOST_TEST)
add_test(NAME remote_nvs COMMAND remote_nvs_tests)
