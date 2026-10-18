/*
 * captive_dns.c - see captive_dns.h. Follows RFC 1035 section 4.
 *
 * Portable: no ESP-IDF headers.
 */

#include "captive_dns.h"

#include <string.h>

#define HEADER_BYTES 12
#define TYPE_A 1
#define TYPE_ANY 255
#define CLASS_IN 1
#define ANSWER_TTL_S 60
#define ANSWER_BYTES 16 /* Name pointer, type, class, TTL, length, address */

#define FLAG_RESPONSE 0x8000
#define FLAG_AUTHORITATIVE 0x0400
#define FLAG_RECURSION_DESIRED 0x0100
#define OPCODE_MASK 0x7800

static uint16_t get_u16(const uint8_t *in)
{
    return (uint16_t)((in[0] << 8) | in[1]);
}

static void put_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)(value & 0xFF);
}

/* Length of the question name at `offset`, including its final zero byte,
 * or 0 if it runs past the packet. Queries do not use compression. */
static size_t name_length(const uint8_t *packet, size_t length, size_t offset)
{
    size_t position = offset;
    while (position < length) {
        const uint8_t label = packet[position];
        if (label == 0) {
            return position - offset + 1;
        }
        if (label > 63) {
            return 0; /* A compression pointer or a reserved label type */
        }
        position += 1 + label;
    }
    return 0;
}

size_t captive_dns_answer(const uint8_t *query, size_t length, const uint8_t address[4],
                          uint8_t *answer, size_t capacity)
{
    if (length < HEADER_BYTES) {
        return 0;
    }
    const uint16_t flags = get_u16(&query[2]);
    if ((flags & FLAG_RESPONSE) != 0 || (flags & OPCODE_MASK) != 0 || get_u16(&query[4]) != 1) {
        return 0;
    }

    const size_t name = name_length(query, length, HEADER_BYTES);
    const size_t question_end = HEADER_BYTES + name + 4; /* Type and class follow the name */
    if (name == 0 || question_end > length) {
        return 0;
    }
    const uint16_t type = get_u16(&query[HEADER_BYTES + name]);
    const uint16_t class_ = get_u16(&query[HEADER_BYTES + name + 2]);
    const int answers = (class_ == CLASS_IN && (type == TYPE_A || type == TYPE_ANY)) ? 1 : 0;

    const size_t total = question_end + (size_t)answers * ANSWER_BYTES;
    if (total > capacity) {
        return 0;
    }

    /* The header and question, echoed; any EDNS record after them dropped. */
    memcpy(answer, query, question_end);
    put_u16(&answer[2], FLAG_RESPONSE | FLAG_AUTHORITATIVE | (flags & FLAG_RECURSION_DESIRED));
    put_u16(&answer[6], (uint16_t)answers);
    put_u16(&answer[8], 0);
    put_u16(&answer[10], 0);

    if (answers == 1) {
        uint8_t *record = &answer[question_end];
        put_u16(&record[0], 0xC000 | HEADER_BYTES); /* "The name in the question" */
        put_u16(&record[2], TYPE_A);
        put_u16(&record[4], CLASS_IN);
        put_u16(&record[6], 0);
        put_u16(&record[8], ANSWER_TTL_S);
        put_u16(&record[10], 4);
        memcpy(&record[12], address, 4);
    }
    return total;
}
