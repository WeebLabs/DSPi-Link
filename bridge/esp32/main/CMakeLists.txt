# main: the boot sequence only (bridge spec 6.5). All behaviour lives in
# components.
idf_component_register(
    SRCS "app_main.c"
    PRIV_REQUIRES board button dspi_uart http_server hub net ota settings setup_api status_led esp_app_format esp_hw_support spi_flash log)

# Bridge code builds with extra warnings, all fatal (bridge spec 16).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Werror)
