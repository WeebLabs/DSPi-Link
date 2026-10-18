# link_codec: Link binary frames and base64 (portable).
idf_component_register(
    SRCS "link_frames.c" "link_base64.c"
    INCLUDE_DIRS "include")

# Bridge code builds with extra warnings, all fatal (bridge spec 16).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Werror)
