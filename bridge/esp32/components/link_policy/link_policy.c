/*
 * link_policy.c - looks commands up in the generated table. Implements the
 * authorization of Link spec 6.3 and 9.3: the hub asks it before sending
 * any CMD to the DSPi. See link_policy.h.
 *
 * Portable: no ESP-IDF headers.
 */

#include "link_policy.h"

#include "link_policy_table.h"

link_class_t link_policy_class(uint16_t id, bool is_get)
{
    size_t low = 0;
    size_t high = LINK_POLICY_TABLE_ROWS;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const link_policy_row_t *row = &LINK_POLICY_TABLE[middle];
        if (row->id == id) {
            return (link_class_t)(is_get ? row->get_class : row->set_class);
        }
        if (row->id < id) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return LINK_CLASS_CONFIG; /* Not listed: admin only until classified */
}

bool link_policy_role_allows(link_role_t role, link_class_t class_)
{
    switch (role) {
    case LINK_ROLE_ADMIN:
        return true;
    case LINK_ROLE_CONTROL:
        return class_ == LINK_CLASS_READ || class_ == LINK_CLASS_CONTROL;
    case LINK_ROLE_VIEWER:
        return class_ == LINK_CLASS_READ;
    }
    return false;
}

const char *link_policy_role_name(link_role_t role)
{
    switch (role) {
    case LINK_ROLE_ADMIN:
        return "admin";
    case LINK_ROLE_CONTROL:
        return "control";
    case LINK_ROLE_VIEWER:
        return "viewer";
    }
    return "unknown";
}
