/*
 * net_machine.h - when the bridge joins its home network and when it shows
 * its setup network.
 *
 * Implements the states of bridge spec 9.1 as a plain state machine. The
 * caller reports what happened (connected, disconnected, a new network to
 * join, phones on the setup network, the time) and then applies the
 * outputs: whether the setup network should be up and whether the station
 * should be connecting. Keeping the decisions here, away from the Wi-Fi
 * driver, lets the host tests walk every path on a fake clock.
 *
 * Portable: no ESP-IDF headers.
 */

#ifndef NET_MACHINE_H
#define NET_MACHINE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NET_STATE_SETUP,      /* No home network saved: the setup network only */
    NET_STATE_CONNECTING, /* Joining the home network */
    NET_STATE_ONLINE,     /* Joined, with an address */
    NET_STATE_FALLBACK,   /* The home network missing for WIFI_FALLBACK_AFTER_MS */
} net_state_t;

/* Progress of joining a newly entered network, for the setup page. */
typedef enum {
    NET_JOIN_IDLE,
    NET_JOIN_CONNECTING,
    NET_JOIN_CONNECTED,
    NET_JOIN_FAILED,
} net_join_t;

typedef enum {
    NET_JOIN_FAILURE_NONE,
    NET_JOIN_FAILURE_WRONG_PASSWORD,
    NET_JOIN_FAILURE_NOT_FOUND,
    NET_JOIN_FAILURE_TIMED_OUT,
} net_join_failure_t;

/* State. Read the outputs; change it only through the functions below. */
typedef struct {
    net_state_t state;
    uint32_t state_since_ms;
    bool has_network; /* A home network is saved or being joined */

    /* Outputs. */
    bool setup_network_on; /* The setup network should be up */
    bool station_wanted;   /* The station should be connecting or connected */
    bool restore_saved;    /* A join failed: go back to the saved network. Clear once done */

    /* The setup network's switch-off rules (bridge spec 9.1 and 9.3). */
    bool off_scheduled;
    uint32_t off_at_ms;
    unsigned setup_clients;
    uint32_t last_client_change_ms;

    /* A join in progress. */
    net_join_t join;
    net_join_failure_t join_failure;
    uint32_t join_started_ms;
    bool join_had_saved; /* There is a saved network to go back to */
    bool join_saw_not_found;
} net_machine_t;

/* Starts in CONNECTING when a network is saved, otherwise in SETUP. */
void net_machine_init(net_machine_t *machine, bool has_saved_network, uint32_t now_ms);

/* The station has an address. */
void net_machine_connected(net_machine_t *machine, uint32_t now_ms);

/* The station lost or could not reach the network. `wrong_password` and
 * `not_found` describe why, when the driver knows. */
void net_machine_disconnected(net_machine_t *machine, uint32_t now_ms, bool wrong_password,
                              bool not_found);

/* New credentials are being tried: the setup page, or hub.wifi.join. */
void net_machine_join(net_machine_t *machine, uint32_t now_ms);

/* How many phones or computers are on the setup network. */
void net_machine_setup_clients(net_machine_t *machine, unsigned count, uint32_t now_ms);

/* The setup page is done: switch the setup network off shortly. */
void net_machine_finish(net_machine_t *machine, uint32_t now_ms);

/* The home network was forgotten (bridge spec 12.1). */
void net_machine_forget(net_machine_t *machine, uint32_t now_ms);

/* Runs the timers: join timeout, fallback, switching the setup network off.
 * Call about once a second. */
void net_machine_tick(net_machine_t *machine, uint32_t now_ms);

/* Names for logs and the setup API. */
const char *net_state_name(net_state_t state);
const char *net_join_name(net_join_t join);
const char *net_join_failure_name(net_join_failure_t failure);

#endif /* NET_MACHINE_H */
