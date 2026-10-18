/*
 * test_link_policy.c - the generated authorization table (Link spec 6.3 and
 * 9.3).
 */

#include "check.h"
#include "link_policy.h"

static void each_direction_has_its_own_class(void)
{
    /* REQ_LIMITER (0x81): SET changes a setting, GET reads the meter. */
    CHECK(link_policy_class(0x81, false) == LINK_CLASS_CONTROL);
    CHECK(link_policy_class(0x81, true) == LINK_CLASS_READ);
}

static void write_as_read_commands_are_not_read(void)
{
    /* REQ_RTA_CONTROL (0x0E) and REQ_CS_MACRO_FIRE (0x25) are GETs that act,
     * so the uart task must never repeat them (bridge spec 7.3). */
    CHECK(link_policy_class(0x0E, true) == LINK_CLASS_CONTROL);
    CHECK(link_policy_class(0x25, true) == LINK_CLASS_CONTROL);
}

static void unlisted_commands_and_directions_are_config(void)
{
    CHECK(link_policy_class(0x00, true) == LINK_CLASS_CONFIG);  /* Not in the table */
    CHECK(link_policy_class(0x7F, false) == LINK_CLASS_CONFIG); /* GET_PLATFORM sent as a SET */
    CHECK(link_policy_class(0x7F, true) == LINK_CLASS_READ);
    CHECK(link_policy_class(0xF0, false) == LINK_CLASS_CONFIG); /* Bootloader entry */
}

static void roles_allow_their_classes(void)
{
    CHECK(link_policy_role_allows(LINK_ROLE_VIEWER, LINK_CLASS_READ));
    CHECK(!link_policy_role_allows(LINK_ROLE_VIEWER, LINK_CLASS_CONTROL));
    CHECK(link_policy_role_allows(LINK_ROLE_CONTROL, LINK_CLASS_CONTROL));
    CHECK(!link_policy_role_allows(LINK_ROLE_CONTROL, LINK_CLASS_CONFIG));
    CHECK(link_policy_role_allows(LINK_ROLE_ADMIN, LINK_CLASS_CONFIG));
}

void test_link_policy(void)
{
    RUN(each_direction_has_its_own_class);
    RUN(write_as_read_commands_are_not_read);
    RUN(unlisted_commands_and_directions_are_config);
    RUN(roles_allow_their_classes);
}
