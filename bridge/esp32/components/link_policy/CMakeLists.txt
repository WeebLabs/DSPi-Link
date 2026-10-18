# link_policy: the authorization table generated from policy/commands.json
# (tools/gen_policy.py) and its lookups (portable).
idf_component_register(
    SRCS "link_policy.c" "link_policy_table.c"
    INCLUDE_DIRS "include")

# Bridge code builds with extra warnings, all fatal (bridge spec 16).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Werror)
