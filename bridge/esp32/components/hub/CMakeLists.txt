# hub: the Link hub state machine (portable: hub.c, hub_commands.c,
# hub_device.c, hub_polls.c) and the hub task that runs it on the ESP32
# (hub_task.c).
idf_component_register(
    SRCS "hub.c" "hub_commands.c" "hub_device.c" "hub_polls.c" "hub_task.c"
    INCLUDE_DIRS "include"
    REQUIRES bridge_config dspi_frame dspi_uart link_codec link_policy settings
    PRIV_REQUIRES board esp_app_format esp_timer freertos heap http_server log net)

# Bridge code builds with extra warnings, all fatal (bridge spec 16).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Werror)
