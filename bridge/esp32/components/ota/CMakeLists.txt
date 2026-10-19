# ota: writes a new firmware image beside the running one, checks it, and
# confirms a new image after a healthy start (bridge spec 14). ESP32 only.
idf_component_register(
    SRCS "ota.c"
    INCLUDE_DIRS "include"
    REQUIRES bridge_config
    PRIV_REQUIRES app_update esp_app_format esp_timer freertos log mbedtls)

# Bridge code builds with extra warnings, all fatal (bridge spec 16).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Werror)
