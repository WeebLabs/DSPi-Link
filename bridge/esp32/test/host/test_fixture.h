/*
 * test_fixture.h - reads the repository's shared fixtures (fixtures/ at the
 * repository root, Link spec 9.3), so the bridge is tested against the same
 * bytes as every other implementation.
 */

#ifndef TEST_FIXTURE_H
#define TEST_FIXTURE_H

#include <stddef.h>
#include <stdint.h>

/* Reads fixtures/<path> into `out` and returns its length, or 0 if the file
 * is missing or larger than `capacity`. */
size_t test_fixture_read(const char *path, uint8_t *out, size_t capacity);

#endif /* TEST_FIXTURE_H */
