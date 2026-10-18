# hub_auth: the fixed PIN, paired clients and rate limits (portable).
idf_component_register(
    SRCS "hub_auth.c"
    INCLUDE_DIRS "include"
    REQUIRES bridge_config)

# Bridge code builds with extra warnings, all fatal (bridge spec 16).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Werror)
