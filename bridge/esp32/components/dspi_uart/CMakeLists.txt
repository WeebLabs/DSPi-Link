# dspi_uart: the UART link engine (portable dspi_link.c) and the uart task
# that runs it on the ESP32 (dspi_uart.c).
idf_component_register(
    SRCS "dspi_link.c" "dspi_uart.c"
    INCLUDE_DIRS "include"
    REQUIRES dspi_frame esp_common
    PRIV_REQUIRES bridge_config esp_driver_uart esp_driver_gpio esp_timer freertos log)

# Bridge code builds with extra warnings, all fatal (bridge spec 16).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Werror)
