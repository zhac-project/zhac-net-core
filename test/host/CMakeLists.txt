# SPDX-License-Identifier: AGPL-3.0-or-later
# Host tests for net-core app-layer serialization safety (CODEX H-01/H-02).
# Compiles the real main/ sources against minimal ESP/NVS/FreeRTOS stubs under
# ASan+UBSan.
cmake_minimum_required(VERSION 3.16)
project(net_core_host_tests C CXX)

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

set(RAINMAKER_GW ${CMAKE_CURRENT_SOURCE_DIR}/../../components/rainmaker_gw)

add_executable(test_rmk_typemap
    test_rmk_typemap.cpp
    ${RAINMAKER_GW}/rmk_typemap.c)
target_include_directories(test_rmk_typemap PRIVATE
    ${RAINMAKER_GW}/include)

# rmk_store.c #includes the host "nvs.h" stub, which pulls in <cstddef>/
# <cstdint> (it was written for this dir's existing C++ consumers, e.g.
# groups_store.cpp) — those headers don't exist for a plain C compile.
# Real ESP-IDF's nvs.h is a plain-C header, so this only matters for the
# host build; compiling this TU as CXX here mirrors the same fix already
# used for rainmaker_gw.c in components/rainmaker_gw/CMakeLists.txt. The
# header's extern "C" guard keeps the exported symbols C-linkage either way.
set_source_files_properties(${RAINMAKER_GW}/rmk_store.c PROPERTIES LANGUAGE CXX)
add_executable(test_rmk_store
    test_rmk_store.cpp
    ${RAINMAKER_GW}/rmk_store.c)
target_include_directories(test_rmk_store PRIVATE
    ${RAINMAKER_GW}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs)

enable_testing()
add_test(NAME json_safety COMMAND json_safety_host)
add_test(NAME rmk_typemap COMMAND test_rmk_typemap)
add_test(NAME rmk_store COMMAND test_rmk_store)
