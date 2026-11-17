/*
 * link_policy_table.h - the generated policy table, private to link_policy.
 *
 * link_policy_table.c (written by tools/gen_policy.py) defines these and
 * link_policy.c reads them. They are data rather than state: constant, made
 * at build time, never written. Everything else reaches them only through
 * link_policy.h.
 */

#ifndef LINK_POLICY_TABLE_H
#define LINK_POLICY_TABLE_H

#include <stddef.h>
#include <stdint.h>

/* The spec version of policy/commands.json the table was made from,
 * recorded as Link spec 9.3 asks. */
extern const char LINK_POLICY_SPEC_VERSION[];

/* One firmware command: its 16-bit ID and the class of each direction, as
 * link_class_t values. A direction the command lacks is config. */
typedef struct {
    uint16_t id;
    uint8_t set_class;
    uint8_t get_class;
} link_policy_row_t;

/* Every classed command, sorted by ID for a binary search. */
extern const link_policy_row_t LINK_POLICY_TABLE[];
extern const size_t LINK_POLICY_TABLE_ROWS;

#endif /* LINK_POLICY_TABLE_H */
