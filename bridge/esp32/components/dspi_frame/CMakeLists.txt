# dspi_frame: the DSPi UART frame encoder, parser and CRC (portable).
idf_component_register(
    SRCS "dspi_frame.c"
    INCLUDE_DIRS "include")

# Bridge code builds with extra warnings, all fatal (bridge spec 16).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Werror)
