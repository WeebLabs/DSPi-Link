# settings: the saved-settings model and rules (portable) and its storage in
# NVS (ESP-IDF).
idf_component_register(
    SRCS "settings_model.c" "settings.c"
    INCLUDE_DIRS "include"
    REQUIRES board esp_common
    PRIV_REQUIRES bridge_config dspi_uart nvs_flash esp_hw_support bootloader_support log)

# Bridge code builds with extra warnings, all fatal (bridge spec 16).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Werror)
