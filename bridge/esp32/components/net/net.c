/*
 * net.c - applies net_machine's decisions to the Wi-Fi driver. See net.h;
 * implements bridge spec 9.1 and 9.2 and the advertisement of 8.8.
 *
 * Everything that changes the Wi-Fi state runs on ESP-IDF's default event
 * task: driver events arrive there, and the public functions post their
 * requests there (NET_EVENTS). The state machine and the credentials are
 * therefore touched by one task only. The status other tasks read is copied
 * out under `status_lock`.
 */

#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bridge_config.h"
#include "captive_dns.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "settings.h"

static const char *TAG = "net";

#define SERVICE_TYPE "_dspi"
#define SERVICE_PROTOCOL "_tcp"
#define SETUP_NETWORK_CHANNEL 1
#define SCAN_MAX_RESULTS 32
#define TICK_PERIOD_MS 1000
#define DNS_TASK_STACK_BYTES 3072
#define DNS_TASK_PRIORITY 4

/* A wrong password usually fails twice in a row; one failure can be a
 * handshake that merely timed out. */
#define PASSWORD_FAILURES_BEFORE_WRONG 2

static const uint32_t RETRY_DELAYS_MS[] = WIFI_RETRY_DELAYS_MS;
#define RETRY_DELAY_COUNT (sizeof(RETRY_DELAYS_MS) / sizeof(RETRY_DELAYS_MS[0]))

/* Requests posted to the event task by the public functions. */
ESP_EVENT_DEFINE_BASE(NET_EVENTS);
enum {
    NET_EVENT_TICK,
    NET_EVENT_JOIN,
    NET_EVENT_FINISH,
};

typedef struct {
    char ssid[SETTINGS_WIFI_SSID_MAX_BYTES + 1];
    char password[SETTINGS_WIFI_PASSWORD_MAX_BYTES + 1];
} credentials_t;

static struct {
    esp_netif_t *station;
    esp_netif_t *setup;
    esp_timer_handle_t retry_timer;
    esp_timer_handle_t tick_timer;
    char hostname[64];
    char setup_ssid[33];

    /* Event task only. */
    net_machine_t machine;
    credentials_t saved;    /* The network that last worked; empty ssid if none */
    credentials_t trying;   /* The network the station is set to */
    bool station_active;    /* The station has been told to connect */
    bool setup_active;      /* The setup network is up */
    bool ignore_disconnect; /* The next disconnect is one we asked for */
    unsigned password_failures;
    size_t retries;

    bool dns_started;
    bool service_added;
    bool clock_started;

    net_status_t status;
    portMUX_TYPE status_lock;
} net_state = {.status_lock = portMUX_INITIALIZER_UNLOCKED};

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---------------------------------------------------------------------------
 * Status
 * ------------------------------------------------------------------------- */

/* Copies the current state out for other tasks. `ip` is NULL to keep the
 * address as it is. */
static void publish_status(bool connected, const char *ip)
{
    wifi_ap_record_t access_point = {0};
    const bool have_ap = connected && esp_wifi_sta_get_ap_info(&access_point) == ESP_OK;

    taskENTER_CRITICAL(&net_state.status_lock);
    net_status_t *status = &net_state.status;
    status->state = net_state.machine.state;
    status->setup_network_on = net_state.setup_active;
    status->join = net_state.machine.join;
    status->join_failure = net_state.machine.join_failure;
    status->connected = connected;
    if (ip != NULL) {
        snprintf(status->ip, sizeof(status->ip), "%s", connected ? ip : "");
    }
    if (have_ap) {
        snprintf(status->ssid, sizeof(status->ssid), "%s", (const char *)access_point.ssid);
        status->rssi = access_point.rssi;
    }
    snprintf(status->saved_ssid, sizeof(status->saved_ssid), "%s", net_state.saved.ssid);
    taskEXIT_CRITICAL(&net_state.status_lock);
}

esp_err_t net_set_hostname(const char *hostname)
{
    esp_netif_set_hostname(net_state.station, hostname);
    taskENTER_CRITICAL(&net_state.status_lock);
    snprintf(net_state.hostname, sizeof(net_state.hostname), "%s", hostname);
    snprintf(net_state.status.hostname, sizeof(net_state.status.hostname), "%s", hostname);
    taskEXIT_CRITICAL(&net_state.status_lock);
    return mdns_hostname_set(hostname);
}

void net_get_status(net_status_t *status)
{
    taskENTER_CRITICAL(&net_state.status_lock);
    *status = net_state.status;
    taskEXIT_CRITICAL(&net_state.status_lock);
}

/* ---------------------------------------------------------------------------
 * The setup network's DNS (bridge spec 9.2)
 * ------------------------------------------------------------------------- */

static void dns_task(void *argument)
{
    (void)argument;
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(CAPTIVE_DNS_PORT),
        .sin_addr.s_addr = inet_addr(net_setup_address()),
    };
    if (sock < 0 || bind(sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
        ESP_LOGE(TAG, "The setup network's DNS could not start");
        if (sock >= 0) {
            close(sock);
        }
        vTaskDelete(NULL);
        return;
    }

    uint8_t address[4];
    memcpy(address, &local.sin_addr.s_addr, sizeof(address));
    uint8_t query[512];
    uint8_t answer[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t from_length = sizeof(from);
        const int length =
            recvfrom(sock, query, sizeof(query), 0, (struct sockaddr *)&from, &from_length);
        if (length <= 0) {
            continue;
        }
        const size_t answer_length =
            captive_dns_answer(query, (size_t)length, address, answer, sizeof(answer));
        if (answer_length > 0) {
            sendto(sock, answer, answer_length, 0, (struct sockaddr *)&from, from_length);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Applying the state machine's decisions (event task only)
 * ------------------------------------------------------------------------- */

/* Copies a string into one of the driver's fixed-size fields. The driver
 * needs no final NUL, so a 32-byte SSID fills its field exactly. */
static void copy_field(uint8_t *field, size_t size, const char *text)
{
    const size_t length = strnlen(text, size);
    memset(field, 0, size);
    memcpy(field, text, length);
}

static void set_station_credentials(const credentials_t *credentials)
{
    wifi_config_t config = {0};
    copy_field(config.sta.ssid, sizeof(config.sta.ssid), credentials->ssid);
    copy_field(config.sta.password, sizeof(config.sta.password), credentials->password);
    /* An empty password is an open network; anything else needs WPA2 or
     * better, so a lookalike open network is never joined instead. */
    config.sta.threshold.authmode =
        credentials->password[0] != '\0' ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &config);
}

static void set_setup_network(bool on)
{
    if (on == net_state.setup_active) {
        return;
    }
    net_state.setup_active = on;
    esp_wifi_set_mode(on ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (on) {
        wifi_config_t config = {0};
        copy_field(config.ap.ssid, sizeof(config.ap.ssid), net_state.setup_ssid);
        config.ap.ssid_len = (uint8_t)strlen(net_state.setup_ssid);
        config.ap.channel = SETUP_NETWORK_CHANNEL; /* Follows the station's once it joins */
        config.ap.authmode = WIFI_AUTH_OPEN;       /* See bridge spec 9.2 */
        config.ap.max_connection = SETUP_NETWORK_MAX_STATIONS;
        esp_wifi_set_config(WIFI_IF_AP, &config);
        if (!net_state.dns_started) {
            net_state.dns_started = xTaskCreate(dns_task, "captive_dns", DNS_TASK_STACK_BYTES, NULL,
                                                DNS_TASK_PRIORITY, NULL) == pdPASS;
        }
    }
    ESP_LOGI(TAG, "Setup network \"%s\" %s", net_state.setup_ssid, on ? "on" : "off");
}

static void start_station(void)
{
    if (net_state.station_active) {
        return;
    }
    net_state.station_active = true;
    net_state.retries = 0;
    ESP_LOGI(TAG, "Joining \"%s\"", net_state.trying.ssid);
    esp_wifi_connect();
}

static void stop_station(void)
{
    if (!net_state.station_active) {
        return;
    }
    net_state.station_active = false;
    net_state.ignore_disconnect = true;
    esp_timer_stop(net_state.retry_timer);
    esp_wifi_disconnect();
}

/* Makes the radio match what the state machine wants. */
static void apply(void)
{
    net_machine_t *machine = &net_state.machine;

    if (machine->restore_saved) {
        machine->restore_saved = false;
        ESP_LOGW(TAG, "Could not join \"%s\" (%s); returning to \"%s\"", net_state.trying.ssid,
                 net_join_failure_name(machine->join_failure), net_state.saved.ssid);
        stop_station();
        net_state.trying = net_state.saved;
        set_station_credentials(&net_state.trying);
    }

    set_setup_network(machine->setup_network_on);
    if (machine->station_wanted) {
        start_station();
    } else {
        stop_station();
    }
    publish_status(net_state.status.connected, NULL);
}

/* Waits 5 s, 10 s, 20 s, then 30 s between attempts (bridge spec 9.1). */
static void schedule_retry(void)
{
    const size_t step =
        net_state.retries < RETRY_DELAY_COUNT ? net_state.retries : RETRY_DELAY_COUNT - 1;
    net_state.retries++;
    esp_timer_stop(net_state.retry_timer);
    esp_timer_start_once(net_state.retry_timer, (uint64_t)RETRY_DELAYS_MS[step] * 1000);
}

static void retry_now(void *argument)
{
    (void)argument;
    if (net_state.station_active) {
        esp_wifi_connect();
    }
}

/* ---------------------------------------------------------------------------
 * Events (event task)
 * ------------------------------------------------------------------------- */

static bool reason_is_password(uint8_t reason)
{
    return reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
}

static bool reason_is_not_found(uint8_t reason)
{
    return reason == WIFI_REASON_NO_AP_FOUND ||
           reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
           reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD ||
           reason == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD;
}

static void on_station_disconnected(const wifi_event_sta_disconnected_t *event)
{
    if (net_state.ignore_disconnect) {
        net_state.ignore_disconnect = false;
        return;
    }
    publish_status(false, "");
    if (!net_state.station_active) {
        return;
    }

    const bool password_like = reason_is_password(event->reason);
    net_state.password_failures = password_like ? net_state.password_failures + 1 : 0;
    const bool wrong_password = net_state.password_failures >= PASSWORD_FAILURES_BEFORE_WRONG;
    ESP_LOGW(TAG, "Not connected to \"%s\" (reason %d)", net_state.trying.ssid, event->reason);

    net_machine_disconnected(&net_state.machine, now_ms(), wrong_password,
                             reason_is_not_found(event->reason));
    apply();
    if (net_state.station_active) {
        schedule_retry();
    }
}

static void on_got_address(const ip_event_got_ip_t *event)
{
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
    net_state.retries = 0;
    net_state.password_failures = 0;

    /* Credentials are saved only once they have worked (Link spec 7.11). */
    if (strcmp(net_state.trying.ssid, net_state.saved.ssid) != 0 ||
        strcmp(net_state.trying.password, net_state.saved.password) != 0) {
        net_state.saved = net_state.trying;
        const esp_err_t err = settings_save_wifi(net_state.saved.ssid, net_state.saved.password);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not save the Wi-Fi network: %s", esp_err_to_name(err));
        }
    }

    net_machine_connected(&net_state.machine, now_ms());
    publish_status(true, ip);
    apply();

    /* Wall-clock time, for when paired clients were created and last seen
     * (bridge spec 8.9). Started once; it keeps itself in step. */
    if (!net_state.clock_started) {
        const esp_sntp_config_t clock = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        net_state.clock_started = esp_netif_sntp_init(&clock) == ESP_OK;
    }
    ESP_LOGI(TAG, "Connected to \"%s\" as %s.local, address %s", net_state.trying.ssid,
             net_state.hostname, ip);
}

static void on_setup_clients_changed(void)
{
    wifi_sta_list_t clients = {0};
    esp_wifi_ap_get_sta_list(&clients);
    net_machine_setup_clients(&net_state.machine, (unsigned)clients.num, now_ms());
    apply();
}

static void on_join_requested(const credentials_t *credentials)
{
    stop_station();
    net_state.trying = *credentials;
    net_state.password_failures = 0;
    set_station_credentials(&net_state.trying);
    net_machine_join(&net_state.machine, now_ms());
    apply();
}

static void on_event(void *argument, esp_event_base_t base, int32_t id, void *data)
{
    (void)argument;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            apply(); /* The driver is ready: connect if a network is wanted */
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            on_station_disconnected(data);
            break;
        case WIFI_EVENT_AP_STACONNECTED:
        case WIFI_EVENT_AP_STADISCONNECTED:
            on_setup_clients_changed();
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        on_got_address(data);
    } else if (base == NET_EVENTS) {
        switch (id) {
        case NET_EVENT_TICK:
            net_machine_tick(&net_state.machine, now_ms());
            apply();
            break;
        case NET_EVENT_JOIN:
            on_join_requested(data);
            break;
        case NET_EVENT_FINISH:
            net_machine_finish(&net_state.machine, now_ms());
            apply();
            break;
        default:
            break;
        }
    }
}

static void post_tick(void *argument)
{
    (void)argument;
    esp_event_post(NET_EVENTS, NET_EVENT_TICK, NULL, 0, 0);
}

/* ---------------------------------------------------------------------------
 * Public functions
 * ------------------------------------------------------------------------- */

esp_err_t net_join(const char *ssid, const char *password)
{
    const size_t ssid_length = strlen(ssid);
    const size_t password_length = strlen(password);
    /* WPA2 passphrases are 8 to 63 characters; 64 means a raw hex key. */
    const bool password_ok =
        password_length == 0 ||
        (password_length >= 8 && password_length <= SETTINGS_WIFI_PASSWORD_MAX_BYTES);
    if (ssid_length == 0 || ssid_length > SETTINGS_WIFI_SSID_MAX_BYTES || !password_ok) {
        return ESP_ERR_INVALID_ARG;
    }
    credentials_t credentials = {0};
    snprintf(credentials.ssid, sizeof(credentials.ssid), "%s", ssid);
    snprintf(credentials.password, sizeof(credentials.password), "%s", password);
    return esp_event_post(NET_EVENTS, NET_EVENT_JOIN, &credentials, sizeof(credentials),
                          pdMS_TO_TICKS(1000));
}

void net_finish_setup(void)
{
    esp_event_post(NET_EVENTS, NET_EVENT_FINISH, NULL, 0, pdMS_TO_TICKS(1000));
}

/* Keeps the strongest entry for each name, strongest first. */
static size_t collect_networks(const wifi_ap_record_t *records, size_t count,
                               net_network_t *networks, size_t capacity)
{
    size_t found = 0;
    for (size_t i = 0; i < count; i++) {
        const char *ssid = (const char *)records[i].ssid;
        if (ssid[0] == '\0') {
            continue; /* Hidden networks cannot be chosen from a list */
        }
        size_t slot = found;
        for (size_t k = 0; k < found; k++) {
            if (strcmp(networks[k].ssid, ssid) == 0) {
                slot = k;
            }
        }
        const bool is_new = slot == found;
        if ((is_new && found == capacity) || (!is_new && networks[slot].rssi >= records[i].rssi)) {
            continue;
        }
        if (is_new) {
            found++;
        }
        snprintf(networks[slot].ssid, sizeof(networks[slot].ssid), "%s", ssid);
        networks[slot].rssi = records[i].rssi;
        networks[slot].secure = records[i].authmode != WIFI_AUTH_OPEN;
    }

    /* Insertion sort by signal strength: a few dozen entries at most. */
    for (size_t i = 1; i < found; i++) {
        const net_network_t moving = networks[i];
        size_t k = i;
        while (k > 0 && networks[k - 1].rssi < moving.rssi) {
            networks[k] = networks[k - 1];
            k--;
        }
        networks[k] = moving;
    }
    return found;
}

size_t net_scan(net_network_t *networks, size_t capacity)
{
    const wifi_scan_config_t config = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&config, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(err));
        return 0;
    }
    uint16_t count = SCAN_MAX_RESULTS;
    wifi_ap_record_t *records = calloc(SCAN_MAX_RESULTS, sizeof(wifi_ap_record_t));
    if (records == NULL) {
        esp_wifi_clear_ap_list();
        return 0;
    }
    err = esp_wifi_scan_get_ap_records(&count, records);
    const size_t found = err == ESP_OK ? collect_networks(records, count, networks, capacity) : 0;
    free(records);
    return found;
}

const char *net_setup_address(void)
{
    return "192.168.4.1"; /* ESP-IDF's default for the access point interface */
}

bool net_socket_is_on_setup_network(int fd)
{
    struct sockaddr_storage local;
    socklen_t length = sizeof(local);
    if (getsockname(fd, (struct sockaddr *)&local, &length) != 0) {
        return false;
    }

    uint32_t address = 0;
    if (local.ss_family == AF_INET) {
        address = ((const struct sockaddr_in *)&local)->sin_addr.s_addr;
    } else if (local.ss_family == AF_INET6) {
        /* The server listens on IPv6, so IPv4 arrives as ::ffff:a.b.c.d. */
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)&local;
        memcpy(&address, &v6->sin6_addr.s6_addr[12], sizeof(address));
    }
    esp_netif_ip_info_t setup_ip;
    return esp_netif_get_ip_info(net_state.setup, &setup_ip) == ESP_OK &&
           address == setup_ip.ip.addr;
}

/* ---------------------------------------------------------------------------
 * mDNS (bridge spec 8.8)
 * ------------------------------------------------------------------------- */

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
 * Start
 * ------------------------------------------------------------------------- */

static esp_err_t start_wifi(void)
{
    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "initialising Wi-Fi");
    /* The bridge keeps credentials itself (bridge spec 11). */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Wi-Fi storage");

    const bool have_saved =
        settings_load_wifi(net_state.saved.ssid, net_state.saved.password) == ESP_OK;
    net_state.trying = net_state.saved;
    net_machine_init(&net_state.machine, have_saved, now_ms());

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Wi-Fi mode");
    set_station_credentials(&net_state.trying);
    set_setup_network(net_state.machine.setup_network_on);
    /* No power saving: it delays every packet by up to the beacon interval. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "Wi-Fi power saving");
    return esp_wifi_start(); /* WIFI_EVENT_STA_START then runs apply() */
}

esp_err_t net_start(const char *hostname)
{
    snprintf(net_state.hostname, sizeof(net_state.hostname), "%s", hostname);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    /* "XXXX" in bridge spec 2: the same digits as the default hostname. */
    snprintf(net_state.setup_ssid, sizeof(net_state.setup_ssid), "DSPi Bridge %02X%02X", mac[4],
             mac[5]);
    snprintf(net_state.status.mac, sizeof(net_state.status.mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(net_state.status.hostname, sizeof(net_state.status.hostname), "%s", hostname);
    snprintf(net_state.status.setup_ssid, sizeof(net_state.status.setup_ssid), "%s",
             net_state.setup_ssid);

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "starting the network stack");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "starting the event loop");
    net_state.station = esp_netif_create_default_wifi_sta();
    net_state.setup = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(net_state.station, hostname); /* The name DHCP reports */

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL),
                        TAG, "Wi-Fi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL),
                        TAG, "IP events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(NET_EVENTS, ESP_EVENT_ANY_ID, on_event, NULL),
                        TAG, "network requests");

    const esp_timer_create_args_t retry = {.callback = retry_now, .name = "wifi_retry"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&retry, &net_state.retry_timer), TAG, "retry timer");
    const esp_timer_create_args_t tick = {.callback = post_tick, .name = "net_tick"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick, &net_state.tick_timer), TAG, "tick timer");

    ESP_RETURN_ON_ERROR(start_wifi(), TAG, "starting Wi-Fi");
    ESP_RETURN_ON_ERROR(
        esp_timer_start_periodic(net_state.tick_timer, (uint64_t)TICK_PERIOD_MS * 1000), TAG,
        "starting the tick");
    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "starting mDNS");
    ESP_RETURN_ON_ERROR(mdns_hostname_set(hostname), TAG, "setting the mDNS hostname");

    if (net_state.machine.state == NET_STATE_SETUP) {
        ESP_LOGI(TAG, "No home network saved: join \"%s\" to set the bridge up",
                 net_state.setup_ssid);
    }
    return ESP_OK;
}
