# http_server: the Link port (WebSocket, info, web pages) and the port 80
# redirect.
idf_component_register(
    SRCS "http_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES bridge_config esp_http_server esp_timer freertos log web_assets)

# Bridge code builds with extra warnings, all fatal (bridge spec 16).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Werror)
