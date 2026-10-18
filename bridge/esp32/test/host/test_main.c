/*
 * test_main.c - runs every host test suite and reports the result.
 */

#include <stdio.h>

#include "check.h"

void test_board_tables(void);
void test_settings_model(void);
void test_dspi_frame(void);
void test_dspi_link(void);
void test_link_policy(void);
void test_link_frames(void);
void test_hub(void);
void test_net(void);
void test_ws_header(void);

int main(void)
{
    test_board_tables();
    test_settings_model();
    test_dspi_frame();
    test_dspi_link();
    test_link_policy();
    test_link_frames();
    test_hub();
    test_net();
    test_ws_header();

    const int failures = check_failure_count();
    if (failures == 0) {
        printf("All host tests passed.\n");
        return 0;
    }
    printf("%d check(s) failed.\n", failures);
    return 1;
}
