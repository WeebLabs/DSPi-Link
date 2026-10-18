# web_assets: the built web pages (js/apps/web/dist), gzipped into the
# firmware by tools/embed_web.py at build time (bridge spec 10.5).
idf_component_register(
    SRCS "web_assets.c"
    INCLUDE_DIRS "include")

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_get_property(python PYTHON)
    set(dist_dir "${CMAKE_CURRENT_LIST_DIR}/../../../../js/apps/web/dist")
    set(embed_tool "${CMAKE_CURRENT_LIST_DIR}/../../tools/embed_web.py")
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/web_assets_data.c")

    if(NOT EXISTS "${dist_dir}/index.html")
        message(FATAL_ERROR "The web pages are not built. Run: cd js && npm ci && npm run build")
    endif()

    file(GLOB_RECURSE dist_files CONFIGURE_DEPENDS "${dist_dir}/*")
    add_custom_command(
        OUTPUT "${generated}"
        COMMAND "${python}" "${embed_tool}" "${dist_dir}" "${generated}"
        DEPENDS ${dist_files} "${embed_tool}"
        VERBATIM)
    target_sources(${COMPONENT_LIB} PRIVATE "${generated}")
endif()

# Bridge code builds with extra warnings, all fatal (bridge spec 16).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Werror)
