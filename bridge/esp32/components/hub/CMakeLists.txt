# hub: the Link hub state machine (portable: every file but hub_task.c) and
# the hub task that runs it on the ESP32 (hub_task.c).
idf_component_register(
    SRCS "hub.c" "hub_admin.c" "hub_commands.c" "hub_config.c" "hub_device.c" "hub_polls.c"
         "hub_task.c"
    INCLUDE_DIRS "include"
    REQUIRES board bridge_config dspi_frame dspi_uart http_server hub_auth link_codec link_policy
             settings
    PRIV_REQUIRES esp_app_format esp_hw_support esp_system esp_timer freertos heap log mbedtls net)

# Bridge code builds with extra warnings, all fatal (bridge spec 16).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Werror)
