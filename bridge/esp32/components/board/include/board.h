/*
 * board.h - facts about the board the firmware is running on.
 *
 * Implements bridge spec 4.1 and 4.4. Safe to call from any task: every
 * function only reads values fixed at build time or at boot.
 */

#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stddef.h>

#include "board_table.h"

/* The pin table for the chip this image was built for. Never NULL. */
const board_table_t *board_table(void);

/* True when the board has PSRAM and ESP-IDF initialised it at boot. */
bool board_has_psram(void);

/* Simultaneous Link sessions this board allows (bridge spec 8.2). */
size_t board_max_sessions(void);

#endif /* BOARD_H */
