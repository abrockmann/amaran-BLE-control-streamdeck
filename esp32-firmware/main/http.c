#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"   /* inet_pton, AF_INET */

#include "mesh_config.h"
#include "wifi_config.h"
#include "wifi.h"

/* Configs written before OTA existed don't define it; empty disables OTA. */
#ifndef OTA_TOKEN
#define OTA_TOKEN ""
#endif

static const char *TAG = "amaran_http";

/* OTA progress that survives software and panic resets (not power loss). On
 * 2026-09-13 an upload ended in reset_reason "panic" without a serial log; these
 * say, after the restart, how far the last upload got and how little stack the
 * HTTP task had left — shown in GET /status. */
#define OTA_TRACE_MAGIC 0x07A5EED5
enum { OTA_STAGE_BEGIN = 1, OTA_STAGE_RECEIVING, OTA_STAGE_VERIFYING, OTA_STAGE_DONE, OTA_STAGE_FAILED };
RTC_NOINIT_ATTR static uint32_t s_ota_magic;
RTC_NOINIT_ATTR static uint32_t s_ota_stage;
RTC_NOINIT_ATTR static uint32_t s_ota_bytes;
RTC_NOINIT_ATTR static uint32_t s_ota_stack_min;

static void ota_trace(uint32_t stage, uint32_t bytes)
{
    UBaseType_t free_now = uxTaskGetStackHighWaterMark(NULL);
    if (s_ota_magic != OTA_TRACE_MAGIC || stage == OTA_STAGE_BEGIN) {
        s_ota_magic = OTA_TRACE_MAGIC;
        s_ota_stack_min = free_now;
    } else if (free_now < s_ota_stack_min) {
        s_ota_stack_min = free_now;
    }
    s_ota_stage = stage;
    s_ota_bytes = bytes;
}

static const amaran_mqtt_dispatch_t *s_dispatch;
static httpd_handle_t s_server;

/* "all" / "broadcast" / "" → group address. Otherwise match a fixture key
 * or decimal/hex address. Returns 0 on hit, -1 on no match. */
static int resolve_key_or_addr(const char *s, uint16_t *out)
{
    if (s == NULL || *s == '\0' || strcmp(s, "all") == 0 ||
        strcmp(s, "broadcast") == 0) {
        *out = AMARAN_GROUP_ALL;
        return 0;
    }
    char *end;
    long v = strtol(s, &end, 0);
    if (*end == '\0' && v > 0 && v <= 0xffff) {
        *out = (uint16_t)v;
        return 0;
    }
    for (size_t i = 0; i < AMARAN_LIGHT_COUNT; i++) {
        if (strcmp(AMARAN_LIGHTS[i].key, s) == 0) {
            *out = AMARAN_LIGHTS[i].address;
            return 0;
        }
    }
    return -1;
}

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t cap, size_t *out_len)
{
    if (req->content_len == 0) {
        *out_len = 0;
        buf[0] = '\0';
        return ESP_OK;
    }
    if (req->content_len >= cap) return ESP_FAIL;
    int got = httpd_req_recv(req, buf, req->content_len);
    if (got <= 0) return ESP_FAIL;
    buf[got] = '\0';
    *out_len = got;
    return ESP_OK;
}

static esp_err_t send_ok(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t send_onoff(httpd_req_t *req, bool on)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, on ? "{\"ok\":true,\"on\":true}"
                               : "{\"ok\":true,\"on\":false}");
    return ESP_OK;
}

static esp_err_t send_err(httpd_req_t *req, int code, const char *msg)
{
    httpd_resp_set_status(req, code == 404 ? "404 Not Found"
                          : code == 400 ? "400 Bad Request"
                          : "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    char body[128];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", msg);
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

/* Guards against requests a web page can make (security audit, 2026-09-13):
 * - Host: a DNS-rebinding page — its own domain re-resolved to the bridge's IP —
 *   is same-origin to that domain and could read /status and /lights and send
 *   any request. Only an IP literal, DEVICE_ID or DEVICE_ID.local is accepted.
 * - Origin (writes only): browsers send it on cross-site POSTs, and a "simple"
 *   POST needs no preflight, so any page could switch the lights with
 *   fetch(url, {method: "POST", mode: "no-cors"}). curl, the Stream Deck plugin
 *   and scripts send no Origin.
 * Returns true if the request may go on; otherwise the 403 is already sent and
 * the handler must return ESP_FAIL, which closes the socket without reading the
 * rest of a body. */
static bool host_allowed(const char *value)
{
    char host[96];
    strlcpy(host, value, sizeof(host));
    if (host[0] == '[') return true;                 /* IPv6 literal */
    char *colon = strchr(host, ':');
    if (colon) *colon = '\0';                       /* port */
    size_t n = strlen(host);
    if (n > 0 && host[n - 1] == '.') host[--n] = '\0';
    struct in_addr a;
    if (inet_pton(AF_INET, host, &a) == 1) return true;
    size_t id = strlen(DEVICE_ID);
    if (strcasecmp(host, DEVICE_ID) == 0) return true;
    return n == id + 6 && strncasecmp(host, DEVICE_ID, id) == 0 && strcasecmp(host + id, ".local") == 0;
}

static bool request_allowed(httpd_req_t *req, bool write)
{
    char host[96] = {0};
    bool ok = httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK
              && host_allowed(host);
    if (ok && write && httpd_req_get_hdr_value_len(req, "Origin") > 0) ok = false;
    if (ok) return true;
    ESP_LOGW(TAG, "refused %s: host or origin not allowed", req->uri);
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"host or origin not allowed\"}");
    return false;
}

/* POST /refresh — broadcast a status-request to every fixture so the
 * iPad/desktop apps catch up. Mirrors Aaron's Shift+R "refresh all". */
static esp_err_t refresh_all_post(httpd_req_t *req)
{
    if (!request_allowed(req, true)) return ESP_FAIL;
    s_dispatch->refresh(AMARAN_GROUP_ALL);
    return send_ok(req);
}

/* GET /lights — list known fixtures. */
static esp_err_t lights_get(httpd_req_t *req)
{
    if (!request_allowed(req, false)) return ESP_FAIL;
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < AMARAN_LIGHT_COUNT; i++) {
        const amaran_light_t *l = &AMARAN_LIGHTS[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "key", l->key);
        cJSON_AddStringToObject(item, "name", l->name);
        cJSON_AddNumberToObject(item, "address", l->address);
        char mac[18];
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 l->mac[0], l->mac[1], l->mac[2], l->mac[3], l->mac[4], l->mac[5]);
        cJSON_AddStringToObject(item, "mac", mac);
        /* Capabilities from the fixture config, so clients can size their
         * controls (e.g. the Stream Deck color temperature dial) without
         * per-model settings. */
        cJSON_AddBoolToObject(item, "has_color", l->has_color);
        cJSON_AddBoolToObject(item, "has_gm", l->has_gm);
        cJSON_AddNumberToObject(item, "cct_min", l->cct_min);
        cJSON_AddNumberToObject(item, "cct_max", l->cct_max);
        /* Include the state so Stream Deck and switch keys don't have to guess
         * it. Until 2026-09-12 it was missing here, and both kept a local record
         * instead — which went wrong as soon as someone else switched the light. */
        amaran_state_t st;
        if (amaran_mqtt_state_get((int)i, &st)) {
            cJSON_AddBoolToObject(item, "on", st.on);
            cJSON_AddNumberToObject(item, "brightness", st.brightness);
            if (st.is_hs) {
                cJSON_AddStringToObject(item, "mode", "hs");
                cJSON_AddNumberToObject(item, "hue", st.hue);
                cJSON_AddNumberToObject(item, "sat", st.sat);
            } else {
                cJSON_AddStringToObject(item, "mode", "cct");
                cJSON_AddNumberToObject(item, "kelvin", st.kelvin);
            }
            cJSON_AddNumberToObject(item, "gm", st.gm);
        }
        cJSON_AddItemToArray(arr, item);
    }
    char *out = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    free(out);
    cJSON_Delete(arr);
    return ESP_OK;
}

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";    /* also after OTA */
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_watchdog";
    case ESP_RST_TASK_WDT:  return "task_watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
    }
}

/* GET /status — judge the bridge's placement without a USB cable.
 * wifi.rssi: signal to the access point (dBm; below about -70 gets slow).
 * fixtures[].heard/asked: share of status requests a fixture answered, i.e.
 * Bluetooth reach; heard_s_ago stays under ~13 s for a fixture in range
 * (polled every 12 s). onoff: in which round on/off got confirmed. */
static esp_err_t status_get(httpd_req_t *req)
{
    if (!request_allowed(req, false)) return ESP_FAIL;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(root, "reset_reason", reset_reason_str(esp_reset_reason()));
    char sha[10];
    esp_app_get_elf_sha256(sha, sizeof(sha));
    cJSON_AddStringToObject(root, "firmware", esp_app_get_description()->version);
    cJSON_AddStringToObject(root, "elf_sha256", sha);
    const esp_partition_t *running = esp_ota_get_running_partition();
    cJSON_AddStringToObject(root, "partition", running ? running->label : "?");
    /* Rollback state of the running image: "pending_verify" for the first
     * minute after an OTA, then "valid"; "untracked" after a USB flash. */
    esp_ota_img_states_t img_state;
    const char *img = "untracked";
    if (running && esp_ota_get_state_partition(running, &img_state) == ESP_OK) {
        img = img_state == ESP_OTA_IMG_VALID ? "valid"
            : img_state == ESP_OTA_IMG_PENDING_VERIFY ? "pending_verify"
            : img_state == ESP_OTA_IMG_NEW ? "new"
            : img_state == ESP_OTA_IMG_UNDEFINED ? "undefined" : "invalid";
    }
    cJSON_AddStringToObject(root, "image_state", img);
    /* Set after the bootloader rolled back from a broken image. */
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    if (invalid) cJSON_AddStringToObject(root, "rolled_back_from", invalid->label);
    else cJSON_AddNullToObject(root, "rolled_back_from");
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", esp_get_minimum_free_heap_size());
    /* TLS needs contiguous blocks; the total free heap alone can look fine
     * while no single allocation of the size mbedTLS asks for fits. */
    cJSON_AddNumberToObject(root, "largest_free_block", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    cJSON_AddBoolToObject(root, "mqtt_connected", amaran_mqtt_is_connected());
    cJSON_AddNumberToObject(root, "mqtt_connects", amaran_mqtt_connects());
    cJSON_AddBoolToObject(root, "mqtt_tls", amaran_mqtt_tls());

    /* RTC memory is garbage after power-on; trust it only with the magic. */
    if (s_ota_magic == OTA_TRACE_MAGIC && esp_reset_reason() != ESP_RST_POWERON) {
        static const char *stages[] = { "?", "begin", "receiving", "verifying", "done", "failed" };
        cJSON *ota = cJSON_AddObjectToObject(root, "last_ota");
        cJSON_AddStringToObject(ota, "stage", s_ota_stage <= OTA_STAGE_FAILED ? stages[s_ota_stage] : "?");
        cJSON_AddNumberToObject(ota, "bytes", s_ota_bytes);
        cJSON_AddNumberToObject(ota, "stack_free_min", s_ota_stack_min);
    }

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        char bssid[18];
        snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
                 ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
        cJSON_AddNumberToObject(wifi, "rssi", ap.rssi);
        cJSON_AddNumberToObject(wifi, "channel", ap.primary);
        cJSON_AddStringToObject(wifi, "bssid", bssid);
    } else {
        cJSON_AddBoolToObject(wifi, "connected", false);
    }
    cJSON_AddNumberToObject(wifi, "reconnects", amaran_wifi_reconnects());

    amaran_onoff_diag_t od = { 0 };
    amaran_node_onoff_diag(&od);
    cJSON *onoff = cJSON_AddObjectToObject(root, "onoff");
    cJSON *rounds = cJSON_AddArrayToObject(onoff, "confirmed_in_round");
    for (int r = 0; r < AMARAN_ONOFF_ROUNDS; r++) {
        cJSON_AddItemToArray(rounds, cJSON_CreateNumber(od.confirmed_in_round[r]));
    }
    cJSON_AddNumberToObject(onoff, "not_confirmed", od.not_confirmed);

    cJSON *fx = cJSON_AddArrayToObject(root, "fixtures");
    for (size_t i = 0; i < AMARAN_LIGHT_COUNT; i++) {
        amaran_fixture_diag_t d = { 0 };
        amaran_node_fixture_diag((int)i, &d);
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "key", AMARAN_LIGHTS[i].key);
        cJSON_AddNumberToObject(item, "asked", d.asked);
        cJSON_AddNumberToObject(item, "heard", d.heard);
        if (d.heard_s_ago >= 0) {
            cJSON_AddNumberToObject(item, "heard_s_ago", d.heard_s_ago);
        } else {
            cJSON_AddNullToObject(item, "heard_s_ago");
        }
        cJSON_AddItemToArray(fx, item);
    }

    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

/* Split "/lights/<key>/<action>" into its parts. uri excludes the leading
 * slash already (httpd_req_t->uri starts with '/'). Returns 0 on success. */
static int parse_lights_path(const char *uri, char *key, size_t key_cap,
                             char *action, size_t action_cap)
{
    /* Expect: /lights/<key>/<action> */
    if (strncmp(uri, "/lights/", 8) != 0) return -1;
    const char *p = uri + 8;
    const char *slash = strchr(p, '/');
    if (slash == NULL) return -1;
    size_t klen = slash - p;
    if (klen == 0 || klen >= key_cap) return -1;
    memcpy(key, p, klen);
    key[klen] = '\0';
    const char *q = slash + 1;
    const char *qend = strchr(q, '?');
    if (qend == NULL) qend = q + strlen(q);
    size_t alen = qend - q;
    if (alen == 0 || alen >= action_cap) return -1;
    memcpy(action, q, alen);
    action[alen] = '\0';
    return 0;
}

static esp_err_t lights_post(httpd_req_t *req)
{
    if (!request_allowed(req, true)) return ESP_FAIL;
    char key[32], action[16];
    if (parse_lights_path(req->uri, key, sizeof(key), action, sizeof(action)) != 0) {
        return send_err(req, 400, "bad path");
    }
    uint16_t dst;
    if (resolve_key_or_addr(key, &dst) != 0) {
        return send_err(req, 404, "unknown fixture");
    }

    char body[256];
    size_t bodylen = 0;
    if (read_body(req, body, sizeof(body), &bodylen) != ESP_OK) {
        return send_err(req, 400, "bad body");
    }

    if (strcmp(action, "on") == 0 || strcmp(action, "off") == 0) {
        bool on = strcmp(action, "on") == 0;
        s_dispatch->on_off(dst, on);
        amaran_mqtt_note_onoff(dst, on);
        return send_onoff(req, on);
    }
    /* The bridge knows the state, so the client needs one request instead of
     * read-then-write — and two clients can't race on a stale read. */
    if (strcmp(action, "toggle") == 0) {
        bool on;
        if (!amaran_mqtt_onoff_get(dst, &on)) {
            return send_err(req, 400, "toggle needs a single fixture");
        }
        s_dispatch->on_off(dst, !on);
        amaran_mqtt_note_onoff(dst, !on);
        return send_onoff(req, !on);
    }
    if (strcmp(action, "refresh") == 0) {
        s_dispatch->refresh(dst);
        return send_ok(req);
    }

    /* Remaining actions need a JSON body. */
    cJSON *root = bodylen ? cJSON_Parse(body) : NULL;
    if (root == NULL) {
        return send_err(req, 400, "body must be JSON");
    }

    esp_err_t rc = ESP_OK;
    if (strcmp(action, "brightness") == 0) {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "value");
        if (!cJSON_IsNumber(v)) {
            rc = send_err(req, 400, "value: number 0-100");
        } else {
            int pct = v->valueint;
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            s_dispatch->brightness(dst, pct);
            rc = send_ok(req);
        }
    } else if (strcmp(action, "cct") == 0) {
        cJSON *k = cJSON_GetObjectItemCaseSensitive(root, "kelvin");
        cJSON *i = cJSON_GetObjectItemCaseSensitive(root, "intensity");
        cJSON *g = cJSON_GetObjectItemCaseSensitive(root, "gm");
        if (!cJSON_IsNumber(k) || !cJSON_IsNumber(i)) {
            rc = send_err(req, 400, "need {kelvin, intensity}");
        } else {
            int pct = i->valueint;
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            int gm = cJSON_IsNumber(g) ? g->valueint : 0;
            s_dispatch->cct(dst, k->valueint, pct, gm);
            rc = send_ok(req);
        }
    } else if (strcmp(action, "hsi") == 0) {
        cJSON *h = cJSON_GetObjectItemCaseSensitive(root, "hue");
        cJSON *sat = cJSON_GetObjectItemCaseSensitive(root, "sat");
        cJSON *i = cJSON_GetObjectItemCaseSensitive(root, "intensity");
        if (!cJSON_IsNumber(h) || !cJSON_IsNumber(sat) || !cJSON_IsNumber(i)) {
            rc = send_err(req, 400, "need {hue, sat, intensity}");
        } else {
            int pct = i->valueint;
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            s_dispatch->hsi(dst, h->valueint, sat->valueint, pct);
            rc = send_ok(req);
        }
    } else {
        rc = send_err(req, 404, "unknown action");
    }
    cJSON_Delete(root);
    return rc;
}

/* POST /ota — update the firmware over Wi-Fi.
 *
 * Two locks since the security audit of 2026-09-13. The token keeps strangers
 * from starting an upload; it is a random value of its own. Until then it was
 * derived from the MQTT password, and MQTT runs unencrypted — whoever read the
 * password at a reconnect could compute the token. The second lock is the
 * signature: esp_ota_end only accepts an image signed with the project's key
 * (CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT), so a stolen token alone no longer
 * gets foreign code onto the device.
 *
 * Usage:  curl -X POST --data-binary @firmware.bin \
 *            -H "X-Amaran-Token: <token>" http://<ip>/ota
 */
static esp_err_t ota_post(httpd_req_t *req)
{
    char token[128] = {0};
    if (!request_allowed(req, true)) return ESP_FAIL;
    /* Refusals return ESP_FAIL: with ESP_OK the server would go on reading the
     * announced body — up to Content-Length, as slowly as the sender likes —
     * and the single HTTP task would be blocked meanwhile. */
    if (sizeof(OTA_TOKEN) <= 1) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"ota disabled\"}");
        return ESP_FAIL;
    }
    if (httpd_req_get_hdr_value_str(req, "X-Amaran-Token", token, sizeof(token)) != ESP_OK
        || strcmp(token, OTA_TOKEN) != 0) {
        ESP_LOGW(TAG, "OTA refused: token missing or wrong");
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"token\"}");
        return ESP_FAIL;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no ota partition\"}");
    }
    ESP_LOGI(TAG, "OTA to %s, expecting %d bytes", target->label, req->content_len);
    ota_trace(OTA_STAGE_BEGIN, 0);

    esp_ota_handle_t h = 0;
    if (esp_ota_begin(target, OTA_SIZE_UNKNOWN, &h) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"ota begin\"}");
    }

    char buf[1024];
    int rest = req->content_len, total = 0, timeouts = 0;
    while (rest > 0) {
        int n = httpd_req_recv(req, buf, rest < (int)sizeof(buf) ? rest : (int)sizeof(buf));
        /* A socket timeout is not a broken upload: the radio is shared with
         * the mesh and Wi-Fi can stall for seconds. Same retry as the
         * ESP-IDF upload examples. */
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 5) {
            continue;
        }
        if (n <= 0) {
            ota_trace(OTA_STAGE_FAILED, total);
            esp_ota_abort(h);
            ESP_LOGE(TAG, "OTA aborted after %d bytes (recv=%d)", total, n);
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"recv\"}");
        }
        if (esp_ota_write(h, buf, n) != ESP_OK) {
            ota_trace(OTA_STAGE_FAILED, total);
            esp_ota_abort(h);
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"write\"}");
        }
        total += n; rest -= n;
        ota_trace(OTA_STAGE_RECEIVING, total);
    }
    ota_trace(OTA_STAGE_VERIFYING, total);

    /* esp_ota_end verifies the image's checksum and its signature. Only then may
     * the boot partition switch — otherwise the device would boot half an image,
     * or a foreign one. */
    if (esp_ota_end(h) != ESP_OK || esp_ota_set_boot_partition(target) != ESP_OK) {
        ota_trace(OTA_STAGE_FAILED, total);
        ESP_LOGE(TAG, "OTA image invalid, boot partition unchanged");
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid image\"}");
    }

    ota_trace(OTA_STAGE_DONE, total);
    ESP_LOGI(TAG, "OTA done (%d bytes), restarting into %s", total, target->label);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"restarting\":true}");
    vTaskDelay(pdMS_TO_TICKS(500));   /* let the response go out first */
    esp_restart();
    return ESP_OK;
}

int amaran_http_start(const amaran_mqtt_dispatch_t *dispatch)
{
    s_dispatch = dispatch;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* The default 4096 bytes are too small for OTA: on 2026-09-13 the trace in
     * /status showed only 616 bytes left while receiving, and the upload then
     * crashed (reset_reason "panic") in esp_ota_end, which verifies the image
     * and needs more stack. */
    cfg.stack_size = 8192;
    /* With all sockets taken (7 by default) idle connections would lock every
     * client out, OTA included; purging the least recently used one lets a new
     * request in. */
    cfg.lru_purge_enable = true;
    cfg.server_port = HTTP_PORT;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return -1;
    }

    static const httpd_uri_t r_list = {
        .uri = "/lights",
        .method = HTTP_GET,
        .handler = lights_get,
    };
    static const httpd_uri_t r_post = {
        .uri = "/lights/*",
        .method = HTTP_POST,
        .handler = lights_post,
    };
    static const httpd_uri_t r_refresh_all = {
        .uri = "/refresh",
        .method = HTTP_POST,
        .handler = refresh_all_post,
    };
    static const httpd_uri_t r_status = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_get,
    };
    static const httpd_uri_t r_ota = {
        .uri = "/ota",
        .method = HTTP_POST,
        .handler = ota_post,
    };
    httpd_register_uri_handler(s_server, &r_status);
    httpd_register_uri_handler(s_server, &r_ota);
    httpd_register_uri_handler(s_server, &r_list);
    httpd_register_uri_handler(s_server, &r_post);
    httpd_register_uri_handler(s_server, &r_refresh_all);

    ESP_LOGI(TAG, "http listening on :%d", HTTP_PORT);
    return 0;
}
