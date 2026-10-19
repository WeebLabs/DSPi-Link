/*
 * test_origin.c - whether an Origin header names the bridge (Link spec 4).
 */

#include <stddef.h>

#include "check.h"
#include "origin.h"

#define ADDRESS "192.168.1.58"
#define HOSTNAME "dspi-bridge-1b34"

static void the_bridges_own_pages_are_allowed(void)
{
    CHECK(origin_is_own("http://192.168.1.58:11915", ADDRESS, HOSTNAME));
    CHECK(origin_is_own("http://192.168.1.58", ADDRESS, HOSTNAME));
    CHECK(origin_is_own("http://dspi-bridge-1b34.local:11915", ADDRESS, HOSTNAME));
    CHECK(origin_is_own("HTTP://DSPI-Bridge-1B34.LOCAL:11915", ADDRESS, HOSTNAME));
    CHECK(origin_is_own("https://dspi-bridge-1b34.local", ADDRESS, HOSTNAME));
    CHECK(origin_is_own("http://192.168.4.1", "192.168.4.1", HOSTNAME)); /* Setup network */
}

static void other_pages_are_refused(void)
{
    CHECK(!origin_is_own("http://evil.example", ADDRESS, HOSTNAME));
    CHECK(!origin_is_own("http://evil.example:11915", ADDRESS, HOSTNAME));
    /* DNS rebinding: a hostile name pointed at the bridge's address. */
    CHECK(!origin_is_own("http://rebind.evil.example:11915", ADDRESS, HOSTNAME));
    CHECK(!origin_is_own("http://192.168.1.580", ADDRESS, HOSTNAME));
    CHECK(!origin_is_own("http://192.168.1.5", ADDRESS, HOSTNAME));
    CHECK(!origin_is_own("http://other-bridge.local", ADDRESS, HOSTNAME));
    CHECK(!origin_is_own("http://dspi-bridge-1b34.local.evil.example", ADDRESS, HOSTNAME));
    CHECK(!origin_is_own("http://xdspi-bridge-1b34.local", ADDRESS, HOSTNAME));
    CHECK(!origin_is_own("http://dspi-bridge-1b34", ADDRESS, HOSTNAME));
}

static void malformed_origins_are_refused(void)
{
    CHECK(!origin_is_own("null", ADDRESS, HOSTNAME));
    CHECK(!origin_is_own("", ADDRESS, HOSTNAME));
    CHECK(!origin_is_own(NULL, ADDRESS, HOSTNAME));
    CHECK(!origin_is_own("file://", ADDRESS, HOSTNAME));
    CHECK(!origin_is_own("ftp://192.168.1.58", ADDRESS, HOSTNAME));
    CHECK(!origin_is_own("http://", ADDRESS, HOSTNAME));
    CHECK(!origin_is_own("http://192.168.1.58/path", ADDRESS, HOSTNAME));
    CHECK(!origin_is_own("http://192.168.1.58:", ADDRESS, HOSTNAME));
    CHECK(!origin_is_own("http://192.168.1.58:80x", ADDRESS, HOSTNAME));
    CHECK(!origin_is_own("http://[::1]:11915", ADDRESS, HOSTNAME));
    CHECK(!origin_is_own("http://.local", ADDRESS, ""));
    CHECK(!origin_is_own("http://192.168.1.58", "", HOSTNAME)); /* Address unknown */
}

void test_origin(void)
{
    RUN(the_bridges_own_pages_are_allowed);
    RUN(other_pages_are_refused);
    RUN(malformed_origins_are_refused);
}
