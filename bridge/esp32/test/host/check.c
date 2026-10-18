/*
 * check.c - records and reports host test results. See check.h.
 */

#include "check.h"

#include <stdio.h>

static int failures;

void check_record(bool passed, const char *expression, const char *file, int line)
{
    if (!passed) {
        failures++;
        printf("  FAILED %s:%d: %s\n", file, line, expression);
    }
}

int check_failure_count(void)
{
    return failures;
}

void check_run(const char *name, void (*test)(void))
{
    printf("%s\n", name);
    test();
}
