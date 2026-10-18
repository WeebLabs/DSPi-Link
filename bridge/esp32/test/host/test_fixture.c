/*
 * test_fixture.c - see test_fixture.h. FIXTURES_DIR is set by CMakeLists.txt.
 */

#include "test_fixture.h"

#include <stdio.h>

size_t test_fixture_read(const char *path, uint8_t *out, size_t capacity)
{
    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s/%s", FIXTURES_DIR, path);
    FILE *file = fopen(full_path, "rb");
    if (file == NULL) {
        printf("  missing fixture %s\n", full_path);
        return 0;
    }
    const size_t length = fread(out, 1, capacity, file);
    const int more = fgetc(file);
    fclose(file);
    return more == EOF ? length : 0;
}
