/*
 * net.c - Wi-Fi station and mDNS. See net.h; implements the station side
 * of bridge spec 9.1 and the advertisement of 8.8.
 *
 * Wi-Fi events arrive on ESP-IDF's event task; the status they produce is
 * read from other tasks, so it is guarded by `status_lock`.
 */

#include "net.h"

#include <stdio.h>
#include <string.h>

#include "bridge_config.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "mdns.h"

#if __has_include("local_wifi.h")
#include "local_wifi.h"
#define HAVE_LOCAL_WIFI 1
#else
#define HAVE_LOCAL_WIFI 0
#endif

static const char *TAG = "net";

#define SERVICE_TYPE "_dspi"
#define SERVICE_PROTOCOL "_tcp"

static const uint32_t RETRY_DELAYS_MS[] = WIFI_RETRY_DELAYS_MS;
#define RETRY_DELAY_COUNT (sizeof(RETRY_DELAYS_MS) / sizeof(RETRY_DELAYS_MS[0]))

static struct {
    esp_netif_t *station;
    esp_timer_handle_t retry_timer;
    size_t retries;
    bool service_added;
    net_status_t status;
    portMUX_TYPE status_lock;
} net_state = {.status_lock = portMUX_INITIALIZER_UNLOCKED};

/* ---------------------------------------------------------------------------
 * Station
 * ------------------------------------------------------------------------- */

static void connect_now(void *argument)
{
    (void)argument;
    esp_wifi_connect();
}

/* Waits 5 s, 10 s, 20 s, then 30 s between attempts (bridge spec 9.1). */
static void schedule_retry(void)
{
    const size_t step =
        net_state.retries < RETRY_DELAY_COUNT ? net_state.retries : RETRY_DELAY_COUNT - 1;
    net_state.retries++;
    esp_timer_start_once(net_state.retry_timer, (uint64_t)RETRY_DELAYS_MS[step] * 1000);
}

static void set_connected(bool connected, const char *ip)
{
    wifi_ap_record_t access_point = {0};
    const bool have_ap = connected && esp_wifi_sta_get_ap_info(&access_point) == ESP_OK;

    taskENTER_CRITICAL(&net_state.status_lock);
    net_state.status.connected = connected;
    snprintf(net_state.status.ip, sizeof(net_state.status.ip), "%s", connected ? ip : "");
    if (have_ap) {
        snprintf(net_state.status.ssid, sizeof(net_state.status.ssid), "%s",
                 (const char *)access_point.ssid);
        net_state.status.rssi = access_point.rssi;
    }
    taskEXIT_CRITICAL(&net_state.status_lock);
}

static void on_wifi_event(void *argument, esp_event_base_t base, int32_t id, void *data)
{
    (void)argument;
    (void)base;
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d); retrying", event->reason);
        set_connected(false, NULL);
        schedule_retry();
    }
}

static void on_ip_event(void *argument, esp_event_base_t base, int32_t id, void *data)
{
    (void)argument;
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    const ip_event_got_ip_t *event = data;
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
    net_state.retries = 0;
    set_connected(true, ip);
    ESP_LOGI(TAG, "Connected with address %s", ip);
}

static esp_err_t start_station(const char *hostname)
{
    net_state.station = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(net_state.station, hostname); /* The name DHCP reports */

    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "initialising Wi-Fi");
    /* The bridge keeps credentials itself (bridge spec 11). */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Wi-Fi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Wi-Fi mode");

    wifi_config_t config = {0};
#if HAVE_LOCAL_WIFI
    snprintf((char *)config.sta.ssid, sizeof(config.sta.ssid), "%s", LOCAL_WIFI_SSID);
    snprintf((char *)config.sta.password, sizeof(config.sta.password), "%s", LOCAL_WIFI_PASSWORD);
#else
    ESP_LOGW(TAG, "No Wi-Fi network is set: copy components/net/local_wifi.example.h to "
                  "local_wifi.h and rebuild");
#endif
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG, "Wi-Fi config");
    /* No power saving: it delays every packet by up to the beacon interval. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "Wi-Fi power saving");
    return esp_wifi_start();
}

/* ---------------------------------------------------------------------------
 * mDNS (bridge spec 8.8)
 * ------------------------------------------------------------------------- */

static esp_err_t start_mdns(const char *hostname)
{
    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "starting mDNS");
    return mdns_hostname_set(hostname);
}

esp_err_t net_advertise(const char *instance_name, const net_txt_entry_t *txt, size_t count)
{
    mdns_txt_item_t items[12];
    if (count > sizeof(items) / sizeof(items[0])) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < count; i++) {
        items[i] = (mdns_txt_item_t){.key = txt[i].key, .value = txt[i].value};
    }

    if (!net_state.service_added) {
        ESP_RETURN_ON_ERROR(mdns_service_add(instance_name, SERVICE_TYPE, SERVICE_PROTOCOL,
                                             LINK_TCP_PORT, items, count),
                            TAG, "adding the DNS-SD service");
        net_state.service_added = true;
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(
        mdns_service_instance_name_set(SERVICE_TYPE, SERVICE_PROTOCOL, instance_name), TAG,
        "renaming the DNS-SD service");
    return mdns_service_txt_set(SERVICE_TYPE, SERVICE_PROTOCOL, items, count);
}

/* ---------------------------------------------------------------------------
 * Start and status
 * ------------------------------------------------------------------------- */

esp_err_t net_start(const char *hostname)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(net_state.status.mac, sizeof(net_state.status.mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "starting the network stack");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "starting the event loop");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL), TAG,
        "Wi-Fi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, NULL),
                        TAG, "IP events");

    const esp_timer_create_args_t retry = {.callback = connect_now, .name = "wifi_retry"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&retry, &net_state.retry_timer), TAG, "retry timer");

    ESP_RETURN_ON_ERROR(start_station(hostname), TAG, "starting the station");
    ESP_RETURN_ON_ERROR(start_mdns(hostname), TAG, "starting mDNS");
    ESP_LOGI(TAG, "Joining Wi-Fi as %s.local", hostname);
    return ESP_OK;
}

void net_get_status(net_status_t *status)
{
    taskENTER_CRITICAL(&net_state.status_lock);
    *status = net_state.status;
    taskEXIT_CRITICAL(&net_state.status_lock);
}
