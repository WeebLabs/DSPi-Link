/*
 * fake_dspi.c - see fake_dspi.h.
 */

#include "fake_dspi.h"

#include <string.h>

#include "test_frames.h"

/* The fake answers this long after a request's last byte. */
#define ANSWER_DELAY_MS 1

static const uint8_t SERIAL[16] = "E46058388B1A2E2C";
static const uint8_t PLATFORM[7] = {0x01, 0x01, 0x17, 0x09, 0x01, 0x07, 0x00};
static const uint8_t IFACE_STATUS[8] = {0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
/* enabled, tx 16, rx 17, notify on, 1000000 baud */
static const uint8_t UART_CONFIG[8] = {0x01, 16, 17, 0x01, 0x40, 0x42, 0x0F, 0x00};

void fake_dspi_init(fake_dspi_t *fake, uint32_t baud)
{
    memset(fake, 0, sizeof(*fake));
    fake->now_ms = 1000; /* Not zero, so "time 0" bugs show up */
    fake->device_baud = baud;
    fake->host_baud = baud;
    fake->smallest_gap_ms = UINT32_MAX;
    fake_dspi_can(fake, 0x7E, SERIAL, sizeof(SERIAL));
    fake_dspi_can(fake, 0x7F, PLATFORM, sizeof(PLATFORM));
    fake_dspi_can(fake, 0xF9, IFACE_STATUS, sizeof(IFACE_STATUS));
    fake_dspi_can(fake, 0xF6, UART_CONFIG, sizeof(UART_CONFIG));
}

void fake_dspi_can(fake_dspi_t *fake, uint8_t request, const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < fake->canned_count; i++) {
        if (fake->canned[i].request == request) {
            memcpy(fake->canned[i].data, data, length);
            fake->canned[i].length = length;
            return;
        }
    }
    fake->canned[fake->canned_count].request = request;
    memcpy(fake->canned[fake->canned_count].data, data, length);
    fake->canned[fake->canned_count].length = length;
    fake->canned_count++;
}

void fake_dspi_script(fake_dspi_t *fake, fake_answer_t answer)
{
    fake->answers[fake->answer_count++] = answer;
}

/* Queues bytes that arrive at `ready_ms`. Times never go backwards, so the
 * output stays in arrival order. */
static void send_bytes(fake_dspi_t *fake, const uint8_t *bytes, size_t length, uint32_t ready_ms)
{
    if (fake->output_tail > fake->output_head) {
        const uint32_t previous = fake->output_ready_ms[fake->output_tail - 1];
        if (ready_ms < previous) {
            ready_ms = previous;
        }
    }
    for (size_t i = 0; i < length; i++) {
        fake->output[fake->output_tail] = bytes[i];
        fake->output_ready_ms[fake->output_tail] = ready_ms;
        fake->output_tail++;
    }
}

void fake_dspi_notify(fake_dspi_t *fake, const uint8_t *packet, size_t length, uint32_t delay_ms)
{
    uint8_t frame[96];
    const size_t frame_length = test_build_device_frame(0x40, 0, packet, length, frame);
    send_bytes(fake, frame, frame_length, fake->now_ms + delay_ms);
}

static const uint8_t *canned_reply(const fake_dspi_t *fake, uint8_t request, size_t *length)
{
    for (size_t i = 0; i < fake->canned_count; i++) {
        if (fake->canned[i].request == request) {
            *length = fake->canned[i].length;
            return fake->canned[i].data;
        }
    }
    return NULL;
}

/* Answers one complete, valid request. */
static void answer(fake_dspi_t *fake, bool is_get, uint8_t request, uint16_t length)
{
    fake_answer_t script = {.kind = FAKE_ANSWER_NORMALLY};
    if (fake->answer_next < fake->answer_count) {
        script = fake->answers[fake->answer_next++];
    }
    if (script.kind == FAKE_STAY_SILENT) {
        return;
    }

    const uint8_t type = is_get ? 0x82 : 0x81;
    uint8_t status = 0x00;
    const uint8_t *payload = NULL;
    size_t payload_length = 0;

    if (script.kind == FAKE_ANSWER_WITH_STATUS) {
        status = script.status;
    } else if (is_get) {
        payload = canned_reply(fake, request, &payload_length);
        if (payload == NULL) {
            status = 0x02; /* ERROR: unknown command */
        } else if (length != 0 && payload_length > length) {
            payload_length = length; /* The firmware clamps to wLength */
        }
    }

    uint8_t frame[128];
    const size_t frame_length =
        test_build_device_frame(type, status, payload, payload_length, frame);
    if (script.kind == FAKE_ANSWER_CORRUPT) {
        frame[frame_length - 1] ^= 0xFF;
    }
    const uint32_t delay = script.kind == FAKE_ANSWER_LATE ? script.delay_ms : ANSWER_DELAY_MS;
    send_bytes(fake, frame, frame_length, fake->now_ms + delay);
    fake->last_answer_end_ms = fake->now_ms + delay;
}

/* Called once a whole request has arrived in `input`. */
static void handle_request(fake_dspi_t *fake, size_t total)
{
    const uint8_t *in = fake->input;
    const bool is_get = in[1] == 0x02;
    const uint16_t length = (uint16_t)(in[7] | (in[8] << 8));
    const size_t payload_length = is_get ? 0 : length;

    const uint16_t crc = (uint16_t)(in[total - 2] | (in[total - 1] << 8));
    if (crc != test_reference_crc(&in[1], 8 + payload_length)) {
        return; /* A real DSPi answers CRC_ERROR; no test relies on it */
    }

    const uint32_t gap = fake->now_ms - fake->last_answer_end_ms;
    if (fake->record_count > 0 && gap < fake->smallest_gap_ms) {
        fake->smallest_gap_ms = gap;
    }
    if (fake->record_count < FAKE_DSPI_MAX_RECORDED) {
        fake->records[fake->record_count] = (fake_request_record_t){
            .is_get = is_get, .request = in[2], .length = length, .arrived_ms = fake->now_ms};
    }
    fake->record_count++;
    memcpy(fake->last_payload, &in[9], payload_length);
    fake->last_payload_length = payload_length;

    answer(fake, is_get, in[2], length);
}

/* Bytes of the request in `input` once it is complete, or 0 if more are
 * needed. */
static size_t complete_request_length(const fake_dspi_t *fake)
{
    if (fake->input_length < 9) {
        return 0;
    }
    const bool is_get = fake->input[1] == 0x02;
    const size_t length = (size_t)(fake->input[7] | (fake->input[8] << 8));
    const size_t total = 9 + (is_get ? 0 : length) + 2;
    return fake->input_length >= total ? total : 0;
}

static bool port_write(void *context, const uint8_t *data, size_t length)
{
    fake_dspi_t *fake = context;
    if (fake->host_baud != fake->device_baud) {
        return true; /* At the wrong rate the DSPi hears only noise */
    }
    for (size_t i = 0; i < length; i++) {
        if (fake->input_length == 0 && data[i] != 0xA5) {
            continue; /* Wait for a sync byte */
        }
        fake->input[fake->input_length++] = data[i];
        const size_t total = complete_request_length(fake);
        if (total > 0) {
            handle_request(fake, total);
            fake->input_length = 0;
        }
    }
    return true;
}

static size_t port_read(void *context, uint8_t *data, size_t capacity, uint32_t timeout_ms)
{
    fake_dspi_t *fake = context;
    const uint32_t give_up_at = fake->now_ms + timeout_ms;

    if (fake->output_head == fake->output_tail ||
        fake->output_ready_ms[fake->output_head] > give_up_at) {
        fake->now_ms = give_up_at; /* Nothing arrives in time */
        return 0;
    }

    if (fake->output_ready_ms[fake->output_head] > fake->now_ms) {
        fake->now_ms = fake->output_ready_ms[fake->output_head];
    }
    size_t count = 0;
    while (count < capacity && fake->output_head < fake->output_tail &&
           fake->output_ready_ms[fake->output_head] <= fake->now_ms) {
        data[count++] = fake->output[fake->output_head++];
    }
    return count;
}

static bool port_set_baud(void *context, uint32_t baud)
{
    fake_dspi_t *fake = context;
    fake->host_baud = baud;
    return true;
}

static bool port_restart(void *context)
{
    fake_dspi_t *fake = context;
    fake->restarts++;
    return true;
}

static uint32_t port_now_ms(void *context)
{
    const fake_dspi_t *fake = context;
    return fake->now_ms;
}

dspi_port_t fake_dspi_port(fake_dspi_t *fake)
{
    return (dspi_port_t){
        .context = fake,
        .write = port_write,
        .read = port_read,
        .set_baud = port_set_baud,
        .restart = port_restart,
        .now_ms = port_now_ms,
    };
}
