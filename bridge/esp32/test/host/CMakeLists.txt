# Host tests for the bridge's portable components (bridge spec 17).
#
# Builds the [portable] sources directly, without ESP-IDF, and runs them on
# the development machine or in CI:
#
#   cmake -S test/host -B build-host && cmake --build build-host && ctest --test-dir build-host

cmake_minimum_required(VERSION 3.22)
project(dspi_bridge_host_tests C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)

# cJSON, the same library the firmware gets from the ESP-IDF component
# registry (components/hub/idf_component.yml). Built without our warning
# flags, since it is not our code.
include(FetchContent)
# Populate only downloads; cJSON's own CMake project is not needed.
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_Declare(cjson
    URL https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.19.tar.gz
    URL_HASH SHA256=7fa616e3046edfa7a28a32d5f9eacfd23f92900fe1f8ccd988c1662f30454562
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(cjson)
if(NOT cjson_POPULATED)
    FetchContent_Populate(cjson)
endif()
add_library(cjson STATIC ${cjson_SOURCE_DIR}/cJSON.c)
target_include_directories(cjson PUBLIC ${cjson_SOURCE_DIR})
target_compile_options(cjson PRIVATE -w)

set(COMPONENTS ${CMAKE_CURRENT_LIST_DIR}/../../components)

add_executable(host_tests
    check.c
    test_main.c
    test_board_tables.c
    test_settings_model.c
    test_dspi_frame.c
    test_frames.c
    test_dspi_link.c
    fake_dspi.c
    test_fixture.c
    test_link_policy.c
    test_link_frames.c
    test_link_base64.c
    test_hub.c
    test_net.c
    test_ws_header.c
    test_hub_auth.c
    test_button_led.c
    fake_crypto.c
    fake_hub_io.c
    ${COMPONENTS}/board/board_tables.c
    ${COMPONENTS}/dspi_frame/dspi_frame.c
    ${COMPONENTS}/dspi_uart/dspi_link.c
    ${COMPONENTS}/link_policy/link_policy.c
    ${COMPONENTS}/link_policy/link_policy_table.c
    ${COMPONENTS}/link_codec/link_frames.c
    ${COMPONENTS}/link_codec/link_base64.c
    ${COMPONENTS}/hub/hub.c
    ${COMPONENTS}/hub/hub_commands.c
    ${COMPONENTS}/hub/hub_device.c
    ${COMPONENTS}/hub/hub_polls.c
    ${COMPONENTS}/hub/hub_update.c
    ${COMPONENTS}/hub/hub_admin.c
    ${COMPONENTS}/hub/hub_config.c
    ${COMPONENTS}/net/net_machine.c
    ${COMPONENTS}/net/captive_dns.c
    ${COMPONENTS}/http_server/ws_header.c
    ${COMPONENTS}/hub_auth/hub_auth.c
    ${COMPONENTS}/settings/settings_model.c
    ${COMPONENTS}/button/button_machine.c
    ${COMPONENTS}/status_led/led_pattern.c)

target_include_directories(host_tests PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${COMPONENTS}/bridge_config/include
    ${COMPONENTS}/board/include
    ${COMPONENTS}/button/include
    ${COMPONENTS}/status_led/include
    ${COMPONENTS}/settings/include
    ${COMPONENTS}/dspi_frame/include
    ${COMPONENTS}/dspi_uart/include
    ${COMPONENTS}/link_policy/include
    ${COMPONENTS}/link_codec/include
    ${COMPONENTS}/hub/include
    ${COMPONENTS}/net/include
    ${COMPONENTS}/http_server/include
    ${COMPONENTS}/hub_auth/include
    ${COMPONENTS}/hub)
target_link_libraries(host_tests PRIVATE cjson)

# The shared fixtures at the repository root (Link spec 9.3).
target_compile_definitions(host_tests PRIVATE
    FIXTURES_DIR="${CMAKE_CURRENT_LIST_DIR}/../../../../fixtures")

enable_testing()
add_test(NAME host_tests COMMAND host_tests)
