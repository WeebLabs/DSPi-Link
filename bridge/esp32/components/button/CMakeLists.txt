# button: the BOOT button's hold timing (portable button_machine.c) and the
# timer that samples the pin on the ESP32 (button.c).
idf_component_register(
    SRCS "button_machine.c" "button.c"
    INCLUDE_DIRS "include"
    REQUIRES bridge_config esp_common
    PRIV_REQUIRES esp_driver_gpio esp_timer log)

# Bridge code builds with extra warnings, all fatal (bridge spec 16).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Werror)
