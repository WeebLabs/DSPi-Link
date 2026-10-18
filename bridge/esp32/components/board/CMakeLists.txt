# board: per-chip pin tables (portable) and the glue that picks the table
# for the chip this image was built for (ESP-IDF).
idf_component_register(
    SRCS "board_tables.c" "board.c"
    INCLUDE_DIRS "include"
    REQUIRES bridge_config
    PRIV_REQUIRES esp_psram)

# Bridge code builds with extra warnings, all fatal (bridge spec 16).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Werror)
