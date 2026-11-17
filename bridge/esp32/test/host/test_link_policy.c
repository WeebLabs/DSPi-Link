/*
 * test_link_policy.c - the generated authorization table (Link spec 6.3 and
 * 9.3), keyed by 16-bit command ID.
 */

#include <string.h>

#include "check.h"
#include "link_policy.h"

static void each_direction_has_its_own_class(void)
{
    /* DYN.LIMITER_THRESHOLD (0x0711): SET changes a setting, GET reads it. */
    CHECK(link_policy_class(0x0711, false) == LINK_CLASS_CONTROL);
    CHECK(link_policy_class(0x0711, true) == LINK_CLASS_READ);
    /* OUTPUT.LATENCY (0x0420): a device-global flash write and restart. */
    CHECK(link_policy_class(0x0420, false) == LINK_CLASS_CONFIG);
    CHECK(link_policy_class(0x0420, true) == LINK_CLASS_READ);
}

static void every_get_is_read(void)
{
    /* Firmware 1.1.7 has no write-as-read commands, so every GET the
     * firmware has is a read, including the ones no old opcode reached. */
    CHECK(link_policy_class(0x0110, true) == LINK_CLASS_READ); /* SYS.PROTOCOL_INFO */
    CHECK(link_policy_class(0x0114, true) == LINK_CLASS_READ); /* SYS.LAST_RESULT */
    CHECK(link_policy_class(0x0910, true) == LINK_CLASS_READ); /* MEAS.LEVELS */
    CHECK(link_policy_class(0x0240, true) == LINK_CLASS_READ); /* STORE.ALL_PARAMS */
    CHECK(link_policy_class(0x0924, true) == LINK_CLASS_READ); /* MEAS.RTA_BINS */
}

/* The commands Link's own clients send: control users must be able to. */
static void link_traffic_is_classed_for_control(void)
{
    CHECK(link_policy_class(0x0550, false) == LINK_CLASS_CONTROL); /* ROUTE.MASTER_VOLUME */
    CHECK(link_policy_class(0x0124, false) == LINK_CLASS_CONTROL); /* SYS.DEVICE_NAME */
    CHECK(link_policy_class(0x0240, false) == LINK_CLASS_CONTROL); /* STORE.ALL_PARAMS */
    CHECK(link_policy_class(0x0920, false) == LINK_CLASS_CONTROL); /* MEAS.RTA_CONFIG */
    CHECK(link_policy_class(0x0928, false) == LINK_CLASS_CONTROL); /* MEAS.RTA_START */
    CHECK(link_policy_class(0x0929, false) == LINK_CLASS_CONTROL); /* MEAS.RTA_STOP */
    CHECK(link_policy_class(0x0210, false) == LINK_CLASS_CONTROL); /* STORE.PRESET_SAVE */
}

/*
 * The SETs whose command number belonged to a control-class opcode in
 * firmware 1.1.6. A table keyed by bRequest alone would hand each of them to
 * any control session. Each must be config (firmware migration guide
 * 13.3.8).
 */
static void commands_that_shared_an_old_number_are_config(void)
{
    static const uint16_t ids[] = {
        0x0130, /* SYS.ENTER_BOOTLOADER, number of 0x30 SET_PSYBASS */
        0x0230, /* STORE.FACTORY_RESET */
        0x0216, /* STORE.STARTUP */
        0x0330, /* INPUT.I2S_RX_PIN */
        0x0332, /* INPUT.I2S_RX_RATE */
        0x0342, /* INPUT.ADAT_RX_CLOCK_MODE */
        0x0410, /* OUTPUT.TYPE */
        0x0430, /* OUTPUT.ADAT_TX_ENABLE */
        0x0A18, /* CS.SAVE */
        0x0A30, /* CS.IR_CMD */
        0x0A32, /* CS.IR_LEARN_ARM */
        0x0A70, /* CS.DISPLAY_CONFIG */
        0x0B10, /* XPORT.UART_CONFIG */
        0x0C10, /* CLOCK.I2S_BCK_PIN */
        0x0C12, /* CLOCK.I2S_CLOCK_MODE */
    };
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        CHECK(link_policy_class(ids[i], false) == LINK_CLASS_CONFIG);
    }
}

static void unlisted_commands_and_directions_are_config(void)
{
    CHECK(link_policy_class(0x0000, true) == LINK_CLASS_CONFIG);  /* Page 0 has no commands */
    CHECK(link_policy_class(0x007F, true) == LINK_CLASS_CONFIG);  /* 1.1.6 GET_PLATFORM */
    CHECK(link_policy_class(0x0120, false) == LINK_CLASS_CONFIG); /* SYS.PLATFORM as a SET */
    CHECK(link_policy_class(0x0120, true) == LINK_CLASS_READ);
    CHECK(link_policy_class(0x0130, true) == LINK_CLASS_CONFIG);  /* Bootloader as a GET */
    CHECK(link_policy_class(0x0001, true) == LINK_CLASS_CONFIG);  /* Below the first row */
    CHECK(link_policy_class(0xFFFF, false) == LINK_CLASS_CONFIG); /* Past the last row */
}

static void roles_allow_their_classes(void)
{
    CHECK(link_policy_role_allows(LINK_ROLE_VIEWER, LINK_CLASS_READ));
    CHECK(!link_policy_role_allows(LINK_ROLE_VIEWER, LINK_CLASS_CONTROL));
    CHECK(link_policy_role_allows(LINK_ROLE_CONTROL, LINK_CLASS_CONTROL));
    CHECK(!link_policy_role_allows(LINK_ROLE_CONTROL, LINK_CLASS_CONFIG));
    CHECK(link_policy_role_allows(LINK_ROLE_ADMIN, LINK_CLASS_CONFIG));
}

static void role_names_are_the_wire_names(void)
{
    CHECK_STR_EQ(link_policy_role_name(LINK_ROLE_VIEWER), "viewer");
    CHECK_STR_EQ(link_policy_role_name(LINK_ROLE_ADMIN), "admin");
    /* A corrupt role must not pass for a real one. */
    CHECK_STR_EQ(link_policy_role_name((link_role_t)9), "unknown");
}

static void the_command_id_joins_the_page_and_number(void)
{
    CHECK(link_policy_command_id(0x50, 0x0502) == 0x0550);
    CHECK(link_policy_command_id(0x10, 0x0100) == 0x0110); /* Low byte 0, as the bridge sends */
}

void test_link_policy(void)
{
    RUN(each_direction_has_its_own_class);
    RUN(every_get_is_read);
    RUN(link_traffic_is_classed_for_control);
    RUN(commands_that_shared_an_old_number_are_config);
    RUN(unlisted_commands_and_directions_are_config);
    RUN(roles_allow_their_classes);
    RUN(role_names_are_the_wire_names);
    RUN(the_command_id_joins_the_page_and_number);
}
