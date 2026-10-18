/*
 * net_machine.c - see net_machine.h; implements bridge spec 9.1.
 *
 * Portable: no ESP-IDF headers.
 */

#include "net_machine.h"

#include "bridge_config.h"

/* True once `now` has reached `when`, allowing for the clock wrapping. */
static bool reached(uint32_t now, uint32_t when)
{
    return (int32_t)(now - when) >= 0;
}

static void enter(net_machine_t *machine, net_state_t state, uint32_t now)
{
    machine->state = state;
    machine->state_since_ms = now;
    switch (state) {
    case NET_STATE_SETUP:
        machine->setup_network_on = true;
        machine->station_wanted = false;
        machine->off_scheduled = false;
        break;
    case NET_STATE_CONNECTING:
        machine->station_wanted = true;
        break;
    case NET_STATE_ONLINE:
        machine->station_wanted = true;
        /* Counting starts now: a setup network nobody uses goes off after
         * SETUP_NETWORK_IDLE_OFF_MS. */
        machine->last_client_change_ms = now;
        break;
    case NET_STATE_FALLBACK:
        machine->setup_network_on = true;
        machine->station_wanted = true;
        machine->off_scheduled = false;
        break;
    }
}

/* Switches the setup network off at `when`, unless already due sooner. */
static void schedule_off(net_machine_t *machine, uint32_t when)
{
    if (!machine->off_scheduled || (int32_t)(when - machine->off_at_ms) < 0) {
        machine->off_scheduled = true;
        machine->off_at_ms = when;
    }
}

static void fail_join(net_machine_t *machine, net_join_failure_t failure, uint32_t now)
{
    machine->join = NET_JOIN_FAILED;
    machine->join_failure = failure;
    if (machine->join_had_saved) {
        machine->restore_saved = true; /* Back to the network that worked */
        enter(machine, NET_STATE_CONNECTING, now);
    } else {
        machine->has_network = false;
        enter(machine, NET_STATE_SETUP, now);
    }
}

void net_machine_init(net_machine_t *machine, bool has_saved_network, uint32_t now_ms)
{
    *machine = (net_machine_t){.has_network = has_saved_network};
    enter(machine, has_saved_network ? NET_STATE_CONNECTING : NET_STATE_SETUP, now_ms);
}

void net_machine_connected(net_machine_t *machine, uint32_t now_ms)
{
    if (machine->join == NET_JOIN_CONNECTING) {
        machine->join = NET_JOIN_CONNECTED;
    }
    enter(machine, NET_STATE_ONLINE, now_ms);
    if (machine->setup_network_on) {
        schedule_off(machine, now_ms + SETUP_NETWORK_AUTO_OFF_MS);
    }
}

void net_machine_disconnected(net_machine_t *machine, uint32_t now_ms, bool wrong_password,
                              bool not_found)
{
    if (machine->join == NET_JOIN_CONNECTING) {
        if (wrong_password) {
            fail_join(machine, NET_JOIN_FAILURE_WRONG_PASSWORD, now_ms);
            return;
        }
        machine->join_saw_not_found = machine->join_saw_not_found || not_found;
    }
    if (machine->state == NET_STATE_ONLINE) {
        enter(machine, NET_STATE_CONNECTING, now_ms);
    }
}

void net_machine_join(net_machine_t *machine, uint32_t now_ms)
{
    machine->join = NET_JOIN_CONNECTING;
    machine->join_failure = NET_JOIN_FAILURE_NONE;
    machine->join_started_ms = now_ms;
    machine->join_had_saved = machine->has_network;
    machine->join_saw_not_found = false;
    machine->has_network = true;
    machine->restore_saved = false;
    enter(machine, NET_STATE_CONNECTING, now_ms);
}

void net_machine_setup_clients(net_machine_t *machine, unsigned count, uint32_t now_ms)
{
    machine->setup_clients = count;
    machine->last_client_change_ms = now_ms;
}

void net_machine_finish(net_machine_t *machine, uint32_t now_ms)
{
    if (machine->setup_network_on && machine->state == NET_STATE_ONLINE) {
        schedule_off(machine, now_ms + SETUP_NETWORK_FINISH_DELAY_MS);
    }
}

void net_machine_forget(net_machine_t *machine, uint32_t now_ms)
{
    machine->has_network = false;
    machine->join = NET_JOIN_IDLE;
    machine->restore_saved = false;
    enter(machine, NET_STATE_SETUP, now_ms);
}

void net_machine_tick(net_machine_t *machine, uint32_t now_ms)
{
    if (machine->join == NET_JOIN_CONNECTING &&
        reached(now_ms, machine->join_started_ms + WIFI_JOIN_TIMEOUT_MS)) {
        fail_join(machine,
                  machine->join_saw_not_found ? NET_JOIN_FAILURE_NOT_FOUND
                                              : NET_JOIN_FAILURE_TIMED_OUT,
                  now_ms);
        return;
    }

    if (machine->state == NET_STATE_CONNECTING &&
        reached(now_ms, machine->state_since_ms + WIFI_FALLBACK_AFTER_MS)) {
        enter(machine, NET_STATE_FALLBACK, now_ms);
        return;
    }

    if (machine->state == NET_STATE_ONLINE && machine->setup_network_on) {
        const bool finished = machine->off_scheduled && reached(now_ms, machine->off_at_ms);
        const bool unused =
            machine->setup_clients == 0 &&
            reached(now_ms, machine->last_client_change_ms + SETUP_NETWORK_IDLE_OFF_MS);
        if (finished || unused) {
            machine->setup_network_on = false;
            machine->off_scheduled = false;
        }
    }
}

const char *net_state_name(net_state_t state)
{
    switch (state) {
    case NET_STATE_SETUP:
        return "setup";
    case NET_STATE_CONNECTING:
        return "connecting";
    case NET_STATE_ONLINE:
        return "online";
    case NET_STATE_FALLBACK:
        return "fallback";
    }
    return "unknown";
}

const char *net_join_name(net_join_t join)
{
    switch (join) {
    case NET_JOIN_IDLE:
        return "idle";
    case NET_JOIN_CONNECTING:
        return "connecting";
    case NET_JOIN_CONNECTED:
        return "connected";
    case NET_JOIN_FAILED:
        return "failed";
    }
    return "idle";
}

const char *net_join_failure_name(net_join_failure_t failure)
{
    switch (failure) {
    case NET_JOIN_FAILURE_NONE:
        return "none";
    case NET_JOIN_FAILURE_WRONG_PASSWORD:
        return "wrong_password";
    case NET_JOIN_FAILURE_NOT_FOUND:
        return "not_found";
    case NET_JOIN_FAILURE_TIMED_OUT:
        return "timed_out";
    }
    return "none";
}
