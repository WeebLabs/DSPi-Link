/*
 * net.h - the bridge's Wi-Fi connection and its DNS-SD advertisement.
 *
 * Implements the station side of bridge spec 9.1 and the discovery of 8.8.
 * Phase 3 joins one network whose credentials are compiled in from a
 * git-ignored local_wifi.h; the setup network, the setup page and saved
 * credentials replace that in phase 4.
 *
 * Every function may be called from any task.
 */

#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/* One DNS-SD TXT record entry (Link spec 3.1). */
typedef struct {
    const char *key;
    const char *value;
} net_txt_entry_t;

/* What hub.stats reports about the network (bridge spec 8.7). */
typedef struct {
    bool connected;
    char ssid[33];
    int rssi;
    char ip[16];
    char mac[18];
} net_status_t;

/* Starts Wi-Fi in station mode and mDNS with `hostname`, and keeps
 * reconnecting with the delays of bridge spec 9.1. */
esp_err_t net_start(const char *hostname);

/* Advertises, or updates, the bridge's one _dspi._tcp service on the Link
 * port with this instance name and TXT record. */
esp_err_t net_advertise(const char *instance_name, const net_txt_entry_t *txt, size_t count);

void net_get_status(net_status_t *status);

#endif /* NET_H */
