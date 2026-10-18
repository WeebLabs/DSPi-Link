/*
 * check.h - a minimal test harness for the host tests.
 *
 * A failed CHECK prints the file, line and expression and the run carries
 * on, so one run reports every failure. test_main.c returns a non-zero exit
 * code if any check failed, which fails ctest and CI.
 */

#ifndef CHECK_H
#define CHECK_H

#include <stdbool.h>

/* Records one check; prefer the macros below. */
void check_record(bool passed, const char *expression, const char *file, int line);

/* Number of failed checks so far. */
int check_failure_count(void);

/* Runs one test function and prints its name. */
void check_run(const char *name, void (*test)(void));

#define CHECK(condition) check_record((condition), #condition, __FILE__, __LINE__)
#define CHECK_STR_EQ(actual, expected)                                                             \
    check_record(strcmp((actual), (expected)) == 0, #actual " == " #expected, __FILE__, __LINE__)
#define CHECK_NULL(pointer) check_record((pointer) == NULL, #pointer " is NULL", __FILE__, __LINE__)
#define RUN(test) check_run(#test, test)

#endif /* CHECK_H */
