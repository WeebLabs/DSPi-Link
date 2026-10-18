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
    ${COMPONENTS}/board/board_tables.c
    ${COMPONENTS}/dspi_frame/dspi_frame.c
    ${COMPONENTS}/dspi_uart/dspi_link.c
    ${COMPONENTS}/settings/settings_model.c)

target_include_directories(host_tests PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${COMPONENTS}/bridge_config/include
    ${COMPONENTS}/board/include
    ${COMPONENTS}/settings/include
    ${COMPONENTS}/dspi_frame/include
    ${COMPONENTS}/dspi_uart/include)

enable_testing()
add_test(NAME host_tests COMMAND host_tests)
