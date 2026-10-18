/*
 * fake_hub_io.h - a recording hub_io_t for the hub's host tests.
 *
 * Every message the hub sends, every request it submits to the "uart task"
 * and every close, ping and save is recorded for the test to inspect. Time
 * moves only when the test moves it.
 */

#ifndef FAKE_HUB_IO_H
#define FAKE_HUB_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hub.h"

#define FAKE_IO_MAX_MESSAGES 128
#define FAKE_IO_MAX_SUBMISSIONS 64
#define FAKE_IO_MESSAGE_BYTES 12288

typedef struct {
    int slot;
    bool binary;
    uint8_t data[FAKE_IO_MESSAGE_BYTES];
    size_t length;
} fake_message_t;

typedef struct {
    dspi_request_t request; /* payload points at `payload` below */
    uint8_t payload[LINK_MAX_PAYLOAD_BYTES];
    bool may_retry;
    uint32_t tag;
} fake_submission_t;

typedef struct {
    uint32_t now_ms;

    fake_message_t messages[FAKE_IO_MAX_MESSAGES];
    size_t message_count;
    bool queue_full[HUB_MAX_SESSIONS]; /* Make send_* fail for a slot */

    fake_submission_t submissions[FAKE_IO_MAX_SUBMISSIONS];
    size_t submission_count;

    uint16_t close_code[HUB_MAX_SESSIONS]; /* 0 until closed */
    unsigned pings[HUB_MAX_SESSIONS];

    char saved_hub_name[64];
    char saved_device_serial[17];
    char saved_device_name[64];
    unsigned advertise_count;
} fake_hub_io_t;

/* Clears the record and returns the hub_io_t that writes to it. */
hub_io_t fake_hub_io_init(fake_hub_io_t *fake);

#endif /* FAKE_HUB_IO_H */
