/*
 * test_link_base64.c - base64 for the control plane's `_b64` fields (Link
 * spec 7.1), against the test vectors of RFC 4648 section 10.
 */

#include <stdint.h>
#include <string.h>

#include "check.h"
#include "link_base64.h"

/* Encodes `text` and checks the result, its length and the predicted length. */
static void check_vector(const char *text, const char *expected)
{
    char out[16];
    memset(out, 'x', sizeof(out));
    const size_t length = strlen(text);
    const size_t written = link_base64_encode((const uint8_t *)text, length, out);
    CHECK(written == strlen(expected));
    CHECK(link_base64_encoded_length(length) == strlen(expected));
    CHECK_STR_EQ(out, expected);
}

static void matches_the_rfc_4648_vectors(void)
{
    check_vector("", "");
    check_vector("f", "Zg==");
    check_vector("fo", "Zm8=");
    check_vector("foo", "Zm9v");
    check_vector("foob", "Zm9vYg==");
    check_vector("fooba", "Zm9vYmE=");
    check_vector("foobar", "Zm9vYmFy");
}

static void encodes_every_byte_value(void)
{
    /* The top and bottom of the alphabet, and bytes no text vector has. */
    const uint8_t data[] = {0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFB, 0xEF};
    char out[16];
    CHECK(link_base64_encode(data, sizeof(data), out) == 12);
    CHECK_STR_EQ(out, "AAAA////++8=");
}

void test_link_base64(void)
{
    RUN(matches_the_rfc_4648_vectors);
    RUN(encodes_every_byte_value);
}
