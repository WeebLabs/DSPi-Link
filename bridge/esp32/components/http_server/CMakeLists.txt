# http_server: the Link port (WebSocket, info, web pages) and the port 80
# redirect.
idf_component_register(
    SRCS "http_server.c" "origin.c" "ws_header.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common esp_http_server
    PRIV_REQUIRES bridge_config esp_timer freertos log net web_assets)

# Bridge code builds with extra warnings, all fatal (bridge spec 16).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Werror)
