/*
 * net.c - applies net_machine's decisions to the Wi-Fi driver. See net.h;
 * implements bridge spec 9.1 and 9.2 and the advertisement of 8.8.
 *
 * Everything that changes the Wi-Fi state runs on ESP-IDF's default event
 * task: driver events arrive there, and the public functions and the timers
 * post their requests there (NET_EVENTS). The state machine, the
 * credentials and the station's progress are therefore touched by one task
 * only. The status other tasks read is copied out under `status_lock`.
 *
 * A connection attempt ends in an address or a disconnect event, and every
 * disconnect schedules the next attempt. Should an attempt end without
 * either, or a connect be refused, the tick notices the station sitting idle
 * and schedules one, so the station never stops trying (bridge spec 9.1).
 */

#include "net.h"

#include <ctype.h>
#include <errno.h>
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
#define SNTP_SERVER "pool.ntp.org"

/* WPA2 passphrases are 8 to 63 characters; 64 characters are a raw key in
 * hexadecimal (IEEE 802.11i). */
#define WPA2_PASSPHRASE_MIN_BYTES 8
#define WPA2_PASSPHRASE_MAX_BYTES 63
#define WPA2_RAW_KEY_HEX_DIGITS 64

static const uint32_t RETRY_DELAYS_MS[] = WIFI_RETRY_DELAYS_MS;
#define RETRY_DELAY_COUNT (sizeof(RETRY_DELAYS_MS) / sizeof(RETRY_DELAYS_MS[0]))

/* Requests posted to the event task by the public functions and timers. */
ESP_EVENT_DEFINE_BASE(NET_EVENTS);
enum {
    NET_EVENT_TICK,
    NET_EVENT_RETRY,
    NET_EVENT_JOIN,
    NET_EVENT_FINISH,
};

typedef struct {
    char ssid[SETTINGS_WIFI_SSID_MAX_BYTES + 1];
    char password[SETTINGS_WIFI_PASSWORD_MAX_BYTES + 1];
} credentials_t;

static struct {
    /* Set once by net_start(), then only read. */
    esp_netif_t *station;
    esp_netif_t *setup;
    esp_timer_handle_t retry_timer;
    esp_timer_handle_t tick_timer;
    char setup_ssid[33];

    /* Event task only. */
    net_machine_t machine;
    credentials_t saved;  /* The network that last worked; empty ssid if none */
    credentials_t trying; /* The network the station is set to */
    bool setup_active;    /* The setup network is up */
    bool station_active;  /* The station should be connecting or connected */
    bool attempt_open;    /* A connect is under way, with no outcome yet */
    uint32_t attempt_started_ms;
    bool associated; /* Joined to an access point, address or not */
    unsigned password_failures;
    size_t retries;
    bool dns_started;
    bool clock_started;

    /* Hub task only (net_advertise). */
    bool service_added;

    /* Any task, under status_lock. */
    char hostname[64];
    net_status_t status;
    portMUX_TYPE status_lock;
} net_state = {.status_lock = portMUX_INITIALIZER_UNLOCKED};

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Logs a failed driver call that the caller can carry on without. */
static void warn_on_error(esp_err_t err, const char *what)
{
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s failed: %s", what, esp_err_to_name(err));
    }
}

/* ---------------------------------------------------------------------------
 * Status
 * ------------------------------------------------------------------------- */

/* Copies the current state out for other tasks. `ip` is NULL to keep the
 * address as it is. Event task only. */
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
    } else if (!connected) {
        status->ssid[0] = '\0';
        status->rssi = 0;
    }
    snprintf(status->saved_ssid, sizeof(status->saved_ssid), "%s", net_state.saved.ssid);
    taskEXIT_CRITICAL(&net_state.status_lock);
}

static bool is_connected(void)
{
    taskENTER_CRITICAL(&net_state.status_lock);
    const bool connected = net_state.status.connected;
    taskEXIT_CRITICAL(&net_state.status_lock);
    return connected;
}

esp_err_t net_set_hostname(const char *hostname)
{
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(net_state.station, hostname), TAG,
                        "setting the DHCP hostname");
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

/* Opens the DNS socket on the setup network's address. -1 on failure. */
static int open_dns_socket(struct sockaddr_in *local)
{
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return -1;
    }
    /* A bounded wait, so the task is never blocked for ever. */
    const struct timeval timeout = {
        .tv_sec = CAPTIVE_DNS_RECEIVE_TIMEOUT_MS / 1000,
        .tv_usec = (CAPTIVE_DNS_RECEIVE_TIMEOUT_MS % 1000) * 1000,
    };
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        bind(sock, (struct sockaddr *)local, sizeof(*local)) != 0) {
        close(sock);
        return -1;
    }
    return sock;
}

static void dns_task(void *argument)
{
    (void)argument;
    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(CAPTIVE_DNS_PORT),
        .sin_addr.s_addr = inet_addr(net_setup_address()),
    };
    const int sock = open_dns_socket(&local);
    if (sock < 0) {
        ESP_LOGE(TAG, "The setup network's DNS could not start (errno %d)", errno);
        vTaskDelete(NULL);
        return;
    }

    uint8_t address[4];
    memcpy(address, &local.sin_addr.s_addr, sizeof(address));
    uint8_t query[512]; /* The largest classic DNS message (RFC 1035 2.3.4) */
    uint8_t answer[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t from_length = sizeof(from);
        const int length =
            recvfrom(sock, query, sizeof(query), 0, (struct sockaddr *)&from, &from_length);
        if (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGD(TAG, "DNS receive failed (errno %d)", errno);
            vTaskDelay(pdMS_TO_TICKS(CAPTIVE_DNS_ERROR_BACKOFF_MS));
        }
        if (length <= 0) {
            continue; /* Nobody asked, or an error: wait again */
        }
        const size_t answer_length =
            captive_dns_answer(query, (size_t)length, address, answer, sizeof(answer));
        if (answer_length > 0 &&
            sendto(sock, answer, answer_length, 0, (struct sockaddr *)&from, from_length) < 0) {
            ESP_LOGD(TAG, "DNS answer not sent (errno %d)", errno); /* The phone asks again */
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
    warn_on_error(esp_wifi_set_config(WIFI_IF_STA, &config), "Setting the station's network");
}

static void start_dns_task(void)
{
    if (net_state.dns_started) {
        return;
    }
    net_state.dns_started = xTaskCreate(dns_task, "captive_dns", CAPTIVE_DNS_TASK_STACK_BYTES, NULL,
                                        CAPTIVE_DNS_TASK_PRIORITY, NULL) == pdPASS;
    if (!net_state.dns_started) {
        ESP_LOGE(TAG, "No memory for the setup network's DNS task; phones must open %s by hand",
                 net_setup_address());
    }
}

/* Shows or hides the setup network. Returns the mode change's error; the
 * next apply() tries again. */
static esp_err_t set_setup_network(bool on)
{
    if (on == net_state.setup_active) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(on ? WIFI_MODE_APSTA : WIFI_MODE_STA), TAG,
                        "switching the setup network %s", on ? "on" : "off");
    if (on) {
        wifi_config_t config = {0};
        copy_field(config.ap.ssid, sizeof(config.ap.ssid), net_state.setup_ssid);
        config.ap.ssid_len = (uint8_t)strlen(net_state.setup_ssid);
        config.ap.channel = SETUP_NETWORK_CHANNEL; /* Follows the station's once it joins */
        config.ap.authmode = WIFI_AUTH_OPEN;       /* See bridge spec 9.2 */
        config.ap.max_connection = SETUP_NETWORK_MAX_STATIONS;
        warn_on_error(esp_wifi_set_config(WIFI_IF_AP, &config), "Configuring the setup network");
        start_dns_task();
    }
    net_state.setup_active = on;
    ESP_LOGI(TAG, "Setup network \"%s\" %s", net_state.setup_ssid, on ? "on" : "off");
    return ESP_OK;
}

/* Waits WIFI_RETRY_DELAYS_MS between attempts, the last delay repeating
 * (bridge spec 9.1). The timer posts NET_EVENT_RETRY. */
static void schedule_retry(void)
{
    const size_t step =
        net_state.retries < RETRY_DELAY_COUNT ? net_state.retries : RETRY_DELAY_COUNT - 1;
    net_state.retries++;
    const esp_err_t stopped = esp_timer_stop(net_state.retry_timer);
    if (stopped != ESP_OK && stopped != ESP_ERR_INVALID_STATE) { /* Not running is fine */
        warn_on_error(stopped, "Stopping the Wi-Fi retry timer");
    }
    warn_on_error(
        esp_timer_start_once(net_state.retry_timer, (uint64_t)RETRY_DELAYS_MS[step] * 1000),
        "Scheduling a Wi-Fi retry"); /* If not, the tick schedules one */
}

/* Starts one connection attempt, or schedules the next if the driver
 * refuses, which it does while a scan runs. */
static void connect_station(void)
{
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not start joining \"%s\" (%s); trying again later",
                 net_state.trying.ssid, esp_err_to_name(err));
        net_state.attempt_open = false;
        schedule_retry();
        return;
    }
    net_state.attempt_open = true;
    net_state.attempt_started_ms = now_ms();
}

static void start_station(void)
{
    if (net_state.station_active) {
        return;
    }
    net_state.station_active = true;
    net_state.retries = 0;
    ESP_LOGI(TAG, "Joining \"%s\"", net_state.trying.ssid);
    connect_station();
}

/* Leaves the network. The disconnect event this causes, if any, carries
 * WIFI_REASON_ASSOC_LEAVE and is ignored, so nothing needs remembering. */
static void stop_station(void)
{
    if (!net_state.station_active) {
        return;
    }
    net_state.station_active = false;
    net_state.attempt_open = false;
    const esp_err_t stopped = esp_timer_stop(net_state.retry_timer);
    if (stopped != ESP_OK && stopped != ESP_ERR_INVALID_STATE) {
        warn_on_error(stopped, "Stopping the Wi-Fi retry timer");
    }
    warn_on_error(esp_wifi_disconnect(), "Leaving the network");
    publish_status(false, "");
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

    if (set_setup_network(machine->setup_network_on) != ESP_OK) {
        ESP_LOGD(TAG, "The setup network is changed on the next tick");
    }
    if (machine->station_wanted) {
        start_station();
    } else {
        stop_station();
    }
    publish_status(is_connected(), NULL);
}

/* Once a second: an attempt that never ended, or a station left with no
 * attempt and no retry pending, gets a fresh retry. */
static void watch_station(void)
{
    if (!net_state.station_active || is_connected() || net_state.associated) {
        return;
    }
    const bool attempt_lost = net_state.attempt_open &&
                              now_ms() - net_state.attempt_started_ms >= WIFI_ATTEMPT_TIMEOUT_MS;
    const bool idle = !net_state.attempt_open && !esp_timer_is_active(net_state.retry_timer);
    if (!attempt_lost && !idle) {
        return;
    }
    ESP_LOGW(TAG, "Joining \"%s\" stalled; trying again", net_state.trying.ssid);
    net_state.attempt_open = false;
    if (net_state.machine.state == NET_STATE_ONLINE) {
        /* The link went without a disconnect the machine heard of, so
         * its fallback timer must start now. */
        net_machine_disconnected(&net_state.machine, now_ms(), false, false);
        apply();
    }
    schedule_retry();
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
    net_state.associated = false;
    publish_status(false, "");
    /* The bridge itself left (stop_station): not a failure of the network
     * now being tried, whose own attempt reports for itself. */
    if (event->reason == WIFI_REASON_ASSOC_LEAVE || !net_state.station_active) {
        return;
    }
    net_state.attempt_open = false;

    const bool password_like = reason_is_password(event->reason);
    net_state.password_failures = password_like ? net_state.password_failures + 1 : 0;
    const bool wrong_password = net_state.password_failures >= WIFI_PASSWORD_FAILURES_BEFORE_WRONG;
    ESP_LOGW(TAG, "Not connected to \"%s\" (reason %d)", net_state.trying.ssid, event->reason);

    net_machine_disconnected(&net_state.machine, now_ms(), wrong_password,
                             reason_is_not_found(event->reason));
    apply();
    if (net_state.station_active && !net_state.attempt_open) {
        schedule_retry();
    }
}

static void save_working_credentials(void)
{
    if (strcmp(net_state.trying.ssid, net_state.saved.ssid) == 0 &&
        strcmp(net_state.trying.password, net_state.saved.password) == 0) {
        return;
    }
    net_state.saved = net_state.trying;
    const esp_err_t err = settings_save_wifi(net_state.saved.ssid, net_state.saved.password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not save the Wi-Fi network: %s", esp_err_to_name(err));
    }
}

static void on_got_address(const ip_event_got_ip_t *event)
{
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
    net_state.retries = 0;
    net_state.password_failures = 0;
    net_state.attempt_open = false;

    /* Credentials are saved only once they have worked (Link spec 7.11). */
    save_working_credentials();

    net_machine_connected(&net_state.machine, now_ms());
    publish_status(true, ip);
    apply();

    /* Wall-clock time, for when paired clients were created and last seen
     * (bridge spec 8.9). Started once; it keeps itself in step. */
    if (!net_state.clock_started) {
        const esp_sntp_config_t clock = ESP_NETIF_SNTP_DEFAULT_CONFIG(SNTP_SERVER);
        const esp_err_t err = esp_netif_sntp_init(&clock);
        net_state.clock_started = err == ESP_OK;
        warn_on_error(err, "Starting the clock (SNTP)"); /* The next connection tries again */
    }
    taskENTER_CRITICAL(&net_state.status_lock);
    char hostname[sizeof(net_state.hostname)];
    memcpy(hostname, net_state.hostname, sizeof(hostname));
    taskEXIT_CRITICAL(&net_state.status_lock);
    ESP_LOGI(TAG, "Connected to \"%s\" as %s.local, address %s", net_state.trying.ssid, hostname,
             ip);
}

static void on_setup_clients_changed(void)
{
    wifi_sta_list_t clients = {0};
    const esp_err_t err = esp_wifi_ap_get_sta_list(&clients);
    if (err != ESP_OK) {
        warn_on_error(err, "Counting the setup network's phones");
        return; /* Keep the last count; the next change counts again */
    }
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

static void on_wifi_event(int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_START:
        apply(); /* The driver is ready: connect if a network is wanted */
        break;
    case WIFI_EVENT_STA_CONNECTED:
        net_state.associated = true;
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
}

static void on_request(int32_t id, void *data)
{
    switch (id) {
    case NET_EVENT_TICK:
        net_machine_tick(&net_state.machine, now_ms());
        apply();
        watch_station();
        break;
    case NET_EVENT_RETRY:
        if (net_state.station_active && !net_state.attempt_open) {
            connect_station();
        }
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

static void on_event(void *argument, esp_event_base_t base, int32_t id, void *data)
{
    (void)argument;
    if (base == WIFI_EVENT) {
        on_wifi_event(id, data);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        on_got_address(data);
    } else if (base == NET_EVENTS) {
        on_request(id, data);
    }
}

/* Timer callbacks, on the esp_timer task: they only post to the event
 * task. A post lost to a full queue is made up by the next tick. */
static void post_tick(void *argument)
{
    (void)argument;
    if (esp_event_post(NET_EVENTS, NET_EVENT_TICK, NULL, 0, 0) != ESP_OK) {
        ESP_LOGD(TAG, "Tick skipped: the event queue is full");
    }
}

static void post_retry(void *argument)
{
    (void)argument;
    if (esp_event_post(NET_EVENTS, NET_EVENT_RETRY, NULL, 0, 0) != ESP_OK) {
        ESP_LOGD(TAG, "Retry deferred to the tick: the event queue is full");
    }
}

/* ---------------------------------------------------------------------------
 * Public functions
 * ------------------------------------------------------------------------- */

static bool is_hex_key(const char *text)
{
    for (const char *c = text; *c != '\0'; c++) {
        if (!isxdigit((unsigned char)*c)) {
            return false;
        }
    }
    return true;
}

net_credentials_check_t net_check_credentials(const char *ssid, const char *password)
{
    const size_t ssid_length = strlen(ssid);
    if (ssid_length == 0 || ssid_length > SETTINGS_WIFI_SSID_MAX_BYTES) {
        return NET_CREDENTIALS_BAD_SSID;
    }
    const size_t length = strlen(password);
    const bool passphrase =
        length >= WPA2_PASSPHRASE_MIN_BYTES && length <= WPA2_PASSPHRASE_MAX_BYTES;
    const bool raw_key = length == WPA2_RAW_KEY_HEX_DIGITS && is_hex_key(password);
    return length == 0 || passphrase || raw_key ? NET_CREDENTIALS_OK : NET_CREDENTIALS_BAD_PASSWORD;
}

esp_err_t net_join(const char *ssid, const char *password)
{
    if (net_check_credentials(ssid, password) != NET_CREDENTIALS_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    credentials_t credentials = {0};
    snprintf(credentials.ssid, sizeof(credentials.ssid), "%s", ssid);
    snprintf(credentials.password, sizeof(credentials.password), "%s", password);
    return esp_event_post(NET_EVENTS, NET_EVENT_JOIN, &credentials, sizeof(credentials),
                          pdMS_TO_TICKS(NET_REQUEST_TIMEOUT_MS));
}

esp_err_t net_finish_setup(void)
{
    return esp_event_post(NET_EVENTS, NET_EVENT_FINISH, NULL, 0,
                          pdMS_TO_TICKS(NET_REQUEST_TIMEOUT_MS));
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

/* Runs a blocking scan. The driver refuses while the station is part way
 * through joining, so a refused scan waits for that and tries once more. */
static esp_err_t run_scan(void)
{
    const wifi_scan_config_t config = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&config, true);
    if (err == ESP_ERR_WIFI_STATE) {
        vTaskDelay(pdMS_TO_TICKS(WIFI_SCAN_RETRY_DELAY_MS));
        err = esp_wifi_scan_start(&config, true);
    }
    return err;
}

size_t net_scan(net_network_t *networks, size_t capacity)
{
    esp_err_t err = run_scan();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(err));
        return 0;
    }
    uint16_t count = WIFI_SCAN_MAX_RESULTS;
    wifi_ap_record_t *records = calloc(WIFI_SCAN_MAX_RESULTS, sizeof(wifi_ap_record_t));
    if (records == NULL) {
        ESP_LOGW(TAG, "No memory for the Wi-Fi scan's results");
        warn_on_error(esp_wifi_clear_ap_list(), "Freeing the scan's results");
        return 0;
    }
    err = esp_wifi_scan_get_ap_records(&count, records);
    warn_on_error(err, "Reading the scan's results");
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

    /* The servers listen on IPv6, so IPv4 arrives as ::ffff:a.b.c.d. A real
     * IPv6 address is never the setup network's, which has none. */
    static const uint8_t V4_MAPPED_PREFIX[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    uint32_t address = 0;
    if (local.ss_family == AF_INET) {
        address = ((const struct sockaddr_in *)&local)->sin_addr.s_addr;
    } else if (local.ss_family == AF_INET6) {
        const uint8_t *bytes = ((const struct sockaddr_in6 *)&local)->sin6_addr.s6_addr;
        if (memcmp(bytes, V4_MAPPED_PREFIX, sizeof(V4_MAPPED_PREFIX)) != 0) {
            return false;
        }
        memcpy(&address, bytes + sizeof(V4_MAPPED_PREFIX), sizeof(address));
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
    mdns_txt_item_t items[NET_MAX_TXT_ENTRIES];
    if (count > NET_MAX_TXT_ENTRIES) {
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
    ESP_RETURN_ON_ERROR(set_setup_network(net_state.machine.setup_network_on), TAG,
                        "the setup network");
    /* No power saving: it delays every packet by up to the beacon interval. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "Wi-Fi power saving");
    return esp_wifi_start(); /* WIFI_EVENT_STA_START then runs apply() */
}

/* Names the setup network and fills in the fixed parts of the status. */
static esp_err_t set_identity(const char *hostname)
{
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), TAG, "reading the MAC address");
    /* "XXXX" in bridge spec 2: the same digits as the default hostname. */
    snprintf(net_state.setup_ssid, sizeof(net_state.setup_ssid), "DSPi Bridge %02X%02X", mac[4],
             mac[5]);
    snprintf(net_state.hostname, sizeof(net_state.hostname), "%s", hostname);
    snprintf(net_state.status.mac, sizeof(net_state.status.mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(net_state.status.hostname, sizeof(net_state.status.hostname), "%s", hostname);
    snprintf(net_state.status.setup_ssid, sizeof(net_state.status.setup_ssid), "%s",
             net_state.setup_ssid);
    return ESP_OK;
}

esp_err_t net_start(const char *hostname)
{
    ESP_RETURN_ON_ERROR(set_identity(hostname), TAG, "naming the bridge");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "starting the network stack");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "starting the event loop");
    net_state.station = esp_netif_create_default_wifi_sta();
    net_state.setup = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(net_state.station != NULL && net_state.setup != NULL, ESP_ERR_NO_MEM, TAG,
                        "creating the network interfaces");
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(net_state.station, hostname), TAG,
                        "setting the name DHCP reports");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL),
                        TAG, "Wi-Fi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL),
                        TAG, "IP events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(NET_EVENTS, ESP_EVENT_ANY_ID, on_event, NULL),
                        TAG, "network requests");

    const esp_timer_create_args_t retry = {.callback = post_retry, .name = "wifi_retry"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&retry, &net_state.retry_timer), TAG, "retry timer");
    const esp_timer_create_args_t tick = {.callback = post_tick, .name = "net_tick"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick, &net_state.tick_timer), TAG, "tick timer");

    ESP_RETURN_ON_ERROR(start_wifi(), TAG, "starting Wi-Fi");
    ESP_RETURN_ON_ERROR(
        esp_timer_start_periodic(net_state.tick_timer, (uint64_t)WIFI_TICK_MS * 1000), TAG,
        "starting the tick");
    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "starting mDNS");
    ESP_RETURN_ON_ERROR(mdns_hostname_set(hostname), TAG, "setting the mDNS hostname");

    if (net_state.machine.state == NET_STATE_SETUP) {
        ESP_LOGI(TAG, "No home network saved: join \"%s\" to set the bridge up",
                 net_state.setup_ssid);
    }
    return ESP_OK;
}
