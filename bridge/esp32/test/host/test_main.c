/*
 * test_main.c - runs every host test suite and reports the result.
 */

#include <stdio.h>

#include "check.h"

void test_board_tables(void);
void test_settings_model(void);

int main(void)
{
    test_board_tables();
    test_settings_model();

    const int failures = check_failure_count();
    if (failures == 0) {
        printf("All host tests passed.\n");
        return 0;
    }
    printf("%d check(s) failed.\n", failures);
    return 1;
}
