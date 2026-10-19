/*
 * fake_dspi.h - a simulated DSPi on a simulated UART, for host tests of
 * dspi_link.
 *
 * It answers requests the way the firmware does, on a virtual clock that
 * only moves when the link waits, so every test is exact and instant. A
 * test scripts how the next requests are answered (normally, not at all,
 * with a bad CRC, late, or with a given status), queues notifications, and
 * then checks what the fake received.
 */

#ifndef FAKE_DSPI_H
#define FAKE_DSPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dspi_link.h"

#define FAKE_DSPI_MAX_PAYLOAD 8192
#define FAKE_DSPI_OUTPUT_BYTES 16384
#define FAKE_DSPI_MAX_ACTIONS 16
#define FAKE_DSPI_MAX_CANNED 8
#define FAKE_DSPI_MAX_RECORDED 32

/* How the fake answers one request. */
typedef enum {
    FAKE_ANSWER_NORMALLY,    /* Canned data for a GET, OK for a SET */
    FAKE_ANSWER_WITH_STATUS, /* The given status and no payload */
    FAKE_STAY_SILENT,        /* No answer at all */
    FAKE_ANSWER_CORRUPT,     /* A normal answer with its CRC damaged */
    FAKE_ANSWER_LATE,        /* A normal answer after `delay_ms` */
} fake_answer_kind_t;

typedef struct {
    fake_answer_kind_t kind;
    uint8_t status;    /* For FAKE_ANSWER_WITH_STATUS */
    uint32_t delay_ms; /* For FAKE_ANSWER_LATE */
} fake_answer_t;

/* What the fake saw of one request. */
typedef struct {
    bool is_get;
    uint8_t request;
    uint16_t length;
    uint32_t arrived_ms;
} fake_request_record_t;

typedef struct {
    uint32_t now_ms;
    uint32_t device_baud; /* The DSPi's rate */
    uint32_t host_baud;   /* The rate the link set */
    unsigned restarts;    /* Restarts the link asked for, failed ones included */
    bool restart_fails;   /* Make every restart report failure */

    /* Bytes on their way to the link, each with the time it arrives. */
    uint8_t output[FAKE_DSPI_OUTPUT_BYTES];
    uint32_t output_ready_ms[FAKE_DSPI_OUTPUT_BYTES];
    size_t output_head;
    size_t output_tail;

    /* The request being received. */
    uint8_t input[FAKE_DSPI_MAX_PAYLOAD + 16];
    size_t input_length;

    /* Scripted answers, used in order; afterwards every answer is normal. */
    fake_answer_t answers[FAKE_DSPI_MAX_ACTIONS];
    size_t answer_count;
    size_t answer_next;

    /* Canned GET replies by command code. */
    struct {
        uint8_t request;
        uint8_t data[64];
        size_t length;
    } canned[FAKE_DSPI_MAX_CANNED];
    size_t canned_count;

    /* What arrived. */
    fake_request_record_t records[FAKE_DSPI_MAX_RECORDED];
    size_t record_count;
    uint8_t last_payload[FAKE_DSPI_MAX_PAYLOAD];
    size_t last_payload_length;
    uint32_t last_answer_end_ms; /* When the last answer finished arriving */
    uint32_t smallest_gap_ms;    /* Shortest time from an answer to the next request */
} fake_dspi_t;

/* A DSPi at `baud` with serial, platform, interface status and UART
 * configuration answers already canned. */
void fake_dspi_init(fake_dspi_t *fake, uint32_t baud);

/* The port a dspi_link uses to talk to this fake. */
dspi_port_t fake_dspi_port(fake_dspi_t *fake);

/* Sets the reply to GET `request`. */
void fake_dspi_can(fake_dspi_t *fake, uint8_t request, const uint8_t *data, size_t length);

/* Scripts how the next unscripted request is answered. */
void fake_dspi_script(fake_dspi_t *fake, fake_answer_t answer);

/* Sends a notification frame `delay_ms` from now. */
void fake_dspi_notify(fake_dspi_t *fake, const uint8_t *packet, size_t length, uint32_t delay_ms);

/* Puts raw bytes on the line `delay_ms` from now: noise, or a hand-made
 * damaged frame. */
void fake_dspi_send_raw(fake_dspi_t *fake, const uint8_t *bytes, size_t length, uint32_t delay_ms);

#endif /* FAKE_DSPI_H */
