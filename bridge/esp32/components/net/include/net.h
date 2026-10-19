/*
 * net.h - the bridge's Wi-Fi: its home network, its setup network, and its
 * DNS-SD advertisement.
 *
 * Implements bridge spec 9.1 and 9.2 and the advertisement of 8.8. The
 * decisions (when to join, when to show the setup network) live in the
 * portable net_machine.h; this module applies them to the Wi-Fi driver,
 * runs the setup network's DNS answers (captive_dns.h) and keeps the home
 * network's credentials in settings.
 *
 * Each function says which task may call it. Wi-Fi changes are carried
 * out on ESP-IDF's event task, one at a time.
 */

#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "net_machine.h"

/* One DNS-SD TXT record entry (Link spec 3.1). */
typedef struct {
    const char *key;
    const char *value;
} net_txt_entry_t;

/* The network as the setup API and hub.stats report it. */
typedef struct {
    net_state_t state;
    bool setup_network_on;
    net_join_t join;
    net_join_failure_t join_failure;
    bool connected;
    char saved_ssid[33]; /* "" when no network is saved */
    char ssid[33];       /* The network joined, when connected */
    int rssi;
    char ip[16];
    char mac[18];
    char hostname[64];
    char setup_ssid[33]; /* "DSPi Bridge XXXX" */
} net_status_t;

/* One network found by a scan. */
typedef struct {
    char ssid[33];
    int rssi;
    bool secure;
} net_network_t;

/* Why net_check_credentials() refuses a network's name and password. */
typedef enum {
    NET_CREDENTIALS_OK,
    NET_CREDENTIALS_BAD_SSID,     /* Empty, or longer than 32 bytes */
    NET_CREDENTIALS_BAD_PASSWORD, /* Not empty, 8 to 63 characters, or 64 hex digits */
} net_credentials_check_t;

/* Starts Wi-Fi with `hostname`: joins the saved home network, or shows the
 * setup network if none is saved (bridge spec 9.1). Called once, by
 * app_main, after settings. Returns the first step's error, if any. */
esp_err_t net_start(const char *hostname);

/* Advertises, or updates, the bridge's one _dspi._tcp service on the Link
 * port with this instance name and TXT record (at most NET_MAX_TXT_ENTRIES
 * entries). Hub task only. Returns ESP_ERR_INVALID_ARG for too many
 * entries, or mDNS's error. */
esp_err_t net_advertise(const char *instance_name, const net_txt_entry_t *txt, size_t count);

/* Copies the network's state as last published. Any task. */
void net_get_status(net_status_t *status);

/* Takes a new mDNS hostname at once; DHCP reports it from the next lease
 * (bridge spec 8.11). Hub task. Returns the network stack's or mDNS's
 * error. */
esp_err_t net_set_hostname(const char *hostname);

/*
 * Lists nearby 2.4 GHz networks, strongest first and each name once, into
 * `networks` (room for `capacity`), and how many in `found`. Blocks its
 * caller for a few seconds, so call it from a task that may wait (the HTTP
 * server's or the hub's), never the event task. One scan runs at a time; a
 * second caller waits up to WIFI_SCAN_LOCK_WAIT_MS for the first.
 *
 * Returns ESP_OK (with `found` possibly 0: no networks in range),
 * ESP_ERR_TIMEOUT if another scan held the radio too long, or the driver's
 * error. On error `found` is 0.
 */
esp_err_t net_scan_networks(net_network_t *networks, size_t capacity, size_t *found);

/* As net_scan_networks(), returning the count, or 0 if the scan failed. */
size_t net_scan(net_network_t *networks, size_t capacity);

/* Checks a network name and password before anything is changed. Any task. */
net_credentials_check_t net_check_credentials(const char *ssid, const char *password);

/*
 * Tries a new home network. The credentials are saved only once the bridge
 * has an address on it; until then the previous network, if any, is kept to
 * fall back to (Link spec 7.11, bridge spec 9.3). Any task. Returns
 * ESP_ERR_INVALID_ARG for credentials net_check_credentials() refuses, or
 * an error if the request could not be queued within NET_REQUEST_TIMEOUT_MS.
 */
esp_err_t net_join(const char *ssid, const char *password);

/* The setup page has finished: switch the setup network off shortly. Any
 * task. Returns an error if the request could not be queued. */
esp_err_t net_finish_setup(void);

/* True when the socket `fd` was accepted on the setup network rather than
 * the home network: it was sent to the setup network's address, from an
 * address on the setup network's subnet. The setup API answers only there
 * (bridge spec 9.3). False if either address cannot be read. Any task,
 * while the socket is open. */
bool net_socket_is_on_setup_network(int fd);

/* The setup network's address, "192.168.4.1". Any task. */
const char *net_setup_address(void);

#endif /* NET_H */
