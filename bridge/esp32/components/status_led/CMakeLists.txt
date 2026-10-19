# status_led: what the LED shows (portable led_pattern.c) and the driver for
# a plain or addressable LED on the ESP32 (status_led.c).
idf_component_register(
    SRCS "led_pattern.c" "status_led.c"
    INCLUDE_DIRS "include"
    REQUIRES bridge_config button esp_common settings
    PRIV_REQUIRES esp_driver_gpio esp_driver_ledc esp_timer freertos log)

# Bridge code builds with extra warnings, all fatal (bridge spec 16).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Werror)
