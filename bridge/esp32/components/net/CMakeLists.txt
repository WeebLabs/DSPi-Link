# net: the Wi-Fi state machine and DNS answers (portable: net_machine.c,
# captive_dns.c) and the ESP32 side that drives the Wi-Fi driver (net.c).
idf_component_register(
    SRCS "net.c" "net_machine.c" "captive_dns.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES bridge_config esp_wifi esp_netif esp_event esp_timer freertos log lwip settings)

# Bridge code builds with extra warnings, all fatal (bridge spec 16).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Werror)
