# main: the boot sequence only (bridge spec 6.5). All behaviour lives in
# components.
idf_component_register(
    SRCS "app_main.c" "phase2_bench.c"
    PRIV_REQUIRES board settings dspi_uart esp_app_format esp_hw_support esp_timer spi_flash log)

# Bridge code builds with extra warnings, all fatal (bridge spec 16).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Werror)
