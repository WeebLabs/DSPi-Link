# setup_api: the setup network's page and API (bridge spec 9.3).
idf_component_register(
    SRCS "setup_api.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server
    PRIV_REQUIRES board dspi_uart esp_app_format hub log net settings web_assets)

# Bridge code builds with extra warnings, all fatal (bridge spec 16).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Werror)
