#include "wifi.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "wifi_config.h"

static const char *TAG = "amaran_wifi";
#define CONNECTED_BIT (1 << 0)

static EventGroupHandle_t s_evt;
static amaran_wifi_got_ip_cb_t s_got_ip_cb;
static int s_retries = 0;
static uint32_t s_got_ip = 0;   /* IP_EVENT_STA_GOT_IP events since boot */
static esp_netif_t *s_sta;

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t event_id, void *data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        /* An IPv6 link-local address lets mDNS answer AAAA queries. Without it
         * the bridge ignores them, and macOS waited the full 5 s for an IPv6
         * answer on every lookup of <DEVICE_ID>.local (measured 2026-09-13;
         * IPv4-only lookups took 2-6 ms). The HTTP server listens on an IPv6
         * socket and accepts both. */
        esp_netif_create_ip6_linklocal(s_sta);
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_retries++;
        ESP_LOGW(TAG, "wifi disconnected, retry %d", s_retries);
        xEventGroupClearBits(s_evt, CONNECTED_BIT);
        esp_wifi_connect();
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        s_got_ip++;
        xEventGroupSetBits(s_evt, CONNECTED_BIT);
        if (s_got_ip_cb) {
            s_got_ip_cb();
        }
    }
}

int amaran_wifi_start(amaran_wifi_got_ip_cb_t got_ip_cb)
{
    s_got_ip_cb = got_ip_cb;
    s_evt = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    /* Hostname for DHCP, so the router lists the bridge by name. */
    esp_netif_set_hostname(s_sta, DEVICE_ID);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.sta.ssid, WIFI_SSID, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, WIFI_PASSWORD,
            sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    /* Default is WIFI_FAST_SCAN: join the FIRST access point with this SSID.
     * In a mesh with several repeaters that is whichever answers first on the
     * lowest channel — on 2026-09-13 one at -82 dBm, with HTTP round trips of
     * 0.15-5 s while the wired LAN answered in under 1 ms. */
    wifi_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Answer as <DEVICE_ID>.local and announce the HTTP API, so the Stream Deck
     * plugin and scripts work without knowing the IP address. DEVICE_ID must be
     * a valid hostname (letters, digits, hyphens). Not fatal if it fails: the
     * IP address still works. */
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(DEVICE_ID);
        mdns_instance_name_set("amaran ESP32 bridge");
        mdns_service_add(NULL, "_http", "_tcp", HTTP_PORT, NULL, 0);
        ESP_LOGI(TAG, "mdns: %s.local", DEVICE_ID);
    } else {
        ESP_LOGW(TAG, "mdns init failed; use the IP address");
    }

    ESP_LOGI(TAG, "wifi sta starting, ssid=%s", WIFI_SSID);
    return 0;
}

uint32_t amaran_wifi_reconnects(void)
{
    return s_got_ip > 0 ? s_got_ip - 1 : 0;
}

bool amaran_wifi_is_connected(void)
{
    if (s_evt == NULL) return false;
    return (xEventGroupGetBits(s_evt) & CONNECTED_BIT) != 0;
}
