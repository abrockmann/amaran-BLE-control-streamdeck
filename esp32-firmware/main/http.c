#include "http.h"

#include <fcntl.h>
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
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
/* An upload holds the single HTTP task for ~20 s; the health watchdog gives no
 * verdict meanwhile. The counter catches an upload that begins during a check;
 * the time limit keeps a hung upload from switching the watchdog off for good. */
static volatile bool s_ota_busy;
static volatile uint32_t s_ota_gen;
static volatile int64_t s_ota_since_us;
#define OTA_BUSY_MAX_US (5LL * 60 * 1000 * 1000)

static bool ota_holds_http(void)
{
    return s_ota_busy && esp_timer_get_time() - s_ota_since_us < OTA_BUSY_MAX_US;
}

/* ---- sockets ----------------------------------------------------------------
 * One lwIP pool (CONFIG_LWIP_MAX_SOCKETS) serves the HTTP sessions and every
 * other socket. The server purges its least recently used session only once all
 * max_open_sockets sessions are taken; if the pool runs out first, accept()
 * fails and lwIP resets each new connection. With the defaults (pool 10,
 * 7 sessions) that happened on 2026-09-21: the bridge reset every request —
 * /status, /lights, even an unknown path — until it was power-cycled.
 * Reproduced on the device by holding 4 idle connections; closing them brought
 * it back at once. So the pool must hold all sessions plus everything else. */
#define HTTP_MAX_SESSIONS 7
/* Not sessions: the server's own 3 (listen socket + control pair), MQTT, the
 * health check's loopback client, and a reserve of 4 for what isn't listed. */
#define SOCKETS_OUTSIDE_SESSIONS (3 + 1 + 1 + 4)
#if CONFIG_LWIP_MAX_SOCKETS < HTTP_MAX_SESSIONS + SOCKETS_OUTSIDE_SESSIONS
#error "CONFIG_LWIP_MAX_SOCKETS must hold all HTTP sessions plus the other sockets (see http.c)"
#endif

/* Open lwIP sockets, for GET /status: fcntl() fails with EBADF on a free slot. */
static int sockets_open(void)
{
    int n = 0;
    for (int fd = LWIP_SOCKET_OFFSET; fd < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; fd++) {
        if (fcntl(fd, F_GETFL, 0) >= 0) n++;
    }
    return n;
}

static int http_sessions(void)
{
    if (!s_server) return 0;
    int fds[HTTP_MAX_SESSIONS];
    size_t n = HTTP_MAX_SESSIONS;
    return httpd_get_client_list(s_server, &n, fds) == ESP_OK ? (int)n : -1;
}

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

/* ---- confirming a new image ----------------------------------------------
 * OTA rollback (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, since 2026-09-13): a new
 * image boots "pending verify" and has to prove itself before it is marked valid.
 * Until 2026-09-14 the proof was one minute of uptime after the HTTP server
 * started. A review showed that is too little: the task watchdog only logs
 * (CONFIG_ESP_TASK_WDT_PANIC is off), so an image whose HTTP task hangs or that
 * runs out of memory got confirmed all the same — and a confirmed image that
 * can't take the next upload needs the USB cable.
 *
 * Now a task asks the bridge's own HTTP server for /status over loopback (the
 * server and task that must take the next upload) and checks that a block the
 * size of a full TLS record is still free. First check after a minute, so a crash
 * in that minute still rolls back; then every 10 s. An image that hasn't passed
 * after 5 minutes of uptime is marked invalid and the device reboots into the
 * previous one. Starts at boot, not at GOT_IP: an image that never gets onto the
 * network rolls back too. */
#define IMAGE_CHECK_FIRST_US     (60LL * 1000 * 1000)
#define IMAGE_CHECK_INTERVAL_MS  10000
#define IMAGE_CHECK_DEADLINE_US  (5LL * 60 * 1000 * 1000)
/* mbedTLS's input buffer grows to the record size, at most 16 KB. A healthy
 * bridge showed 59 KB as largest_free_block on 2026-09-14. */
#define IMAGE_CHECK_MIN_BLOCK    16384

static struct {
    bool started;              /* this boot began pending_verify */
    uint32_t checks;
    const char *last_error;    /* NULL = last check passed */
    uint32_t stack_free;       /* checker's least free stack, set when it ends */
} s_image_check;

/* The image the bootloader gave up on, read once at boot from the state of the
 * other OTA slot. esp_ota_get_last_invalid_partition() did this per GET /status
 * and validated that image each time (hash and signature over ~1.7 MB), and it
 * returned NULL as soon as the next upload had erased the slot. */
static const char *s_rolled_back_from;

/* ---- health watchdog (since 2026-09-21) --------------------------------------
 * The image check above runs only until an image is confirmed. On 2026-09-21 the
 * bridge stopped answering HTTP and stayed that way until someone pulled the plug;
 * nothing on the bridge noticed. A second task now runs the same check once a
 * minute for the whole uptime, and three failures in a row restart the bridge.
 * What failed and the heap and socket figures of that moment survive the restart
 * (RTC memory; power loss clears them) and appear in GET /status under
 * health.last_restart, so a restart shows up instead of hiding a leak.
 * - Own 5 KB stack: a passing check needs ~2 KB, a failing one plus the restart
 *   record ~2.4 KB (1656 of 4096 left, measured on 2026-09-21), and esp_restart's
 *   shutdown handlers run on it too. The image check's 8 KB are freed after
 *   confirmation as before.
 * - Starts at boot and has to run once before an image is confirmed, so a fault
 *   in it still rolls back. While an image is pending, only the image check acts.
 * - Backoff only for a failure streak that begins within an hour of a boot: each
 *   restart in a row of such restarts doubles the failures needed (3, 6, 12 … up
 *   to 96), so a fault a restart doesn't cure restarts the bridge about every
 *   1.5 h instead of every 4 minutes. A streak that begins after an hour of
 *   uptime always restarts after 3 failures and starts the row anew.
 * - One check at a time (mutex): while an image is pending both tasks check, and
 *   the socket budget counts one loopback client. */
#define HEALTH_FIRST_MS       45000   /* before the image check's first, at 60 s */
#define HEALTH_INTERVAL_MS    60000
#define HEALTH_FAILS_RESTART  3
#define HEALTH_BACKOFF_MAX    5
#define HEALTH_BACKOFF_WINDOW_US (60LL * 60 * 1000 * 1000)
#define HEALTH_STACK          5120
#define CHECK_NOT_RUN         INT32_MIN   /* the HTTP client could not be created */

typedef struct {
    uint32_t magic;
    uint32_t restarts;          /* watchdog restarts since power-on */
    uint32_t backoff;           /* restarts in a row whose streak began early */
    uint32_t uptime_s;          /* the rest describes the last one */
    char     reason[16];
    int32_t  err;               /* esp_http_client_perform result, or CHECK_NOT_RUN */
    int32_t  sock_errno;
    int32_t  http_status;
    uint32_t free_heap, min_free_heap, largest_block;
    int32_t  sockets_open, http_sessions;
    uint32_t stack_free;        /* watchdog task, after the failed checks */
} health_record_t;
RTC_NOINIT_ATTR static health_record_t s_health_rec;
/* The size is part of the magic: a record another layout wrote is dropped. */
#define HEALTH_MAGIC          (0x4EA17D00u ^ (uint32_t)sizeof(health_record_t))

static struct {
    uint32_t checks;            /* checks with a verdict */
    uint32_t fails_in_row;
    int64_t  streak_start_us;   /* uptime at the first failure of the streak */
    const char *last_error;
} s_health;
static TaskHandle_t s_health_task;
static SemaphoreHandle_t s_check_lock;
static volatile bool s_image_pending;

/* The last check's client result, for the restart record. */
static int32_t s_check_err;
static int s_check_errno;
static int s_check_status;

static const char *image_check_once_locked(void)
{
    char url[40];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/status", HTTP_PORT);
    const esp_http_client_config_t cfg = { .url = url, .timeout_ms = 5000 };
    s_check_err = CHECK_NOT_RUN;
    s_check_errno = 0;
    s_check_status = 0;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return "http client";
    const char *err = NULL;
    s_check_err = esp_http_client_perform(c);
    if (s_check_err != ESP_OK) {
        s_check_errno = esp_http_client_get_errno(c);
        err = "http no answer";
    } else if ((s_check_status = esp_http_client_get_status_code(c)) != 200) {
        err = "http status";
    }
    esp_http_client_cleanup(c);
    if (err) return err;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < IMAGE_CHECK_MIN_BLOCK) {
        return "heap";
    }
    return NULL;
}

static const char *image_check_once(void)
{
    if (s_check_lock) xSemaphoreTake(s_check_lock, portMAX_DELAY);
    const char *err = image_check_once_locked();
    if (s_check_lock) xSemaphoreGive(s_check_lock);
    return err;
}

/* For the current streak, or for one that began now. */
static uint32_t health_fails_to_restart(void)
{
    int64_t start = s_health.fails_in_row ? s_health.streak_start_us : esp_timer_get_time();
    if (start >= HEALTH_BACKOFF_WINDOW_US) return HEALTH_FAILS_RESTART;
    uint32_t n = s_health_rec.backoff < HEALTH_BACKOFF_MAX ? s_health_rec.backoff : HEALTH_BACKOFF_MAX;
    return HEALTH_FAILS_RESTART << n;
}

static void health_restart(const char *err)
{
    health_record_t *r = &s_health_rec;
    r->restarts++;
    r->backoff = s_health.streak_start_us >= HEALTH_BACKOFF_WINDOW_US ? 1 : r->backoff + 1;
    r->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    strlcpy(r->reason, err, sizeof(r->reason));
    r->err = s_check_err;
    r->sock_errno = s_check_errno;
    r->http_status = s_check_status;
    r->free_heap = esp_get_free_heap_size();
    r->min_free_heap = esp_get_minimum_free_heap_size();
    r->largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    r->sockets_open = sockets_open();
    r->http_sessions = http_sessions();
    r->stack_free = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGE(TAG, "health check failed %" PRIu32 " times in a row (%s), restarting",
             s_health.fails_in_row, err);
    esp_restart();
}

/* Waits with ulTaskNotifyTake: the image check wakes it early (see there). */
static void health_task(void *arg)
{
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(HEALTH_FIRST_MS));
    for (;;) {
        uint32_t gen = s_ota_gen;
        /* Not before the server runs (no Wi-Fi since boot), and no verdict while
         * an upload holds the HTTP task or began during the check: the check
         * would time out with nothing wrong. */
        if (s_server && !ota_holds_http()) {
            const char *err = image_check_once();
            if (s_ota_gen == gen && !ota_holds_http()) {
                s_health.checks++;
                s_health.last_error = err;
                if (!err) {
                    s_health.fails_in_row = 0;
                } else if (!s_image_pending) {
                    if (s_health.fails_in_row++ == 0) s_health.streak_start_us = esp_timer_get_time();
                    ESP_LOGW(TAG, "health check failed (%" PRIu32 " in a row): %s", s_health.fails_in_row, err);
                    if (s_health.fails_in_row >= health_fails_to_restart()) health_restart(err);
                }
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(HEALTH_INTERVAL_MS));
    }
}

static void image_check_task(void *arg)
{
    const char *label = esp_ota_get_running_partition()->label;
    int64_t deadline = IMAGE_CHECK_DEADLINE_US;
    int64_t wait = IMAGE_CHECK_FIRST_US - esp_timer_get_time();
    if (wait > 0) vTaskDelay(pdMS_TO_TICKS(wait / 1000));

    for (;;) {
        s_image_check.checks++;
        const char *err = image_check_once();
        /* The watchdog has to have run once on this image (see above). Wake it
         * instead of waiting up to a minute for its next turn: a server that
         * came up late would otherwise run into the 5-minute deadline. */
        if (!err && s_health.checks == 0) {
            err = "watchdog not run";
            if (s_health_task) xTaskNotifyGive(s_health_task);
        }
        if (!err) {
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
                s_image_pending = false;
                s_image_check.last_error = NULL;
                ESP_LOGI(TAG, "image on %s confirmed after %" PRIu32 " check(s)", label, s_image_check.checks);
                break;
            }
            err = "mark valid";
        }
        s_image_check.last_error = err;
        ESP_LOGW(TAG, "image check %" PRIu32 " failed: %s", s_image_check.checks, err);
        if (esp_timer_get_time() >= deadline) {
            ESP_LOGE(TAG, "image on %s not confirmed within 5 min (%s), rolling back", label, err);
            esp_err_t rc = esp_ota_mark_app_invalid_rollback_and_reboot();   /* returns only on failure */
            ESP_LOGE(TAG, "rollback failed: %s", esp_err_to_name(rc));
            s_image_check.last_error = "rollback failed";
            deadline += IMAGE_CHECK_DEADLINE_US;   /* keep checking; try again later */
        }
        vTaskDelay(pdMS_TO_TICKS(IMAGE_CHECK_INTERVAL_MS));
    }
    s_image_check.stack_free = uxTaskGetStackHighWaterMark(NULL);
    vTaskDelete(NULL);
}

void amaran_image_check_start(void)
{
    /* RTC memory is garbage after power-on; trust the record only with the magic. */
    if (esp_reset_reason() == ESP_RST_POWERON || s_health_rec.magic != HEALTH_MAGIC) {
        memset(&s_health_rec, 0, sizeof(s_health_rec));
        s_health_rec.magic = HEALTH_MAGIC;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t state;
    if (other && other != running && esp_ota_get_state_partition(other, &state) == ESP_OK
        && (state == ESP_OTA_IMG_INVALID || state == ESP_OTA_IMG_ABORTED)) {
        s_rolled_back_from = other->label;
    }

    s_image_pending = running && esp_ota_get_state_partition(running, &state) == ESP_OK
                      && state == ESP_OTA_IMG_PENDING_VERIFY;
    s_check_lock = xSemaphoreCreateMutex();   /* NULL: checks just don't serialise */
    /* Without it a pending image is never confirmed and rolls back — right for an
     * image that can't spare 5 KB at boot. A confirmed one runs on unwatched. */
    if (xTaskCreate(health_task, "amaran_health", HEALTH_STACK, NULL, 2, &s_health_task) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the health watchdog");
    }

    if (!s_image_pending) {
        return;                       /* flashed over USB, or already confirmed */
    }
    s_image_check.started = true;
    /* Same stack as the HTTP task: the rollback path verifies the other image
     * the way esp_ota_end does, and a stack overflow in the check would roll back
     * a good image. The stack is freed once the image is confirmed. */
    if (xTaskCreate(image_check_task, "amaran_imgchk", 8192, NULL, 2, NULL) != pdPASS) {
        /* Not even this fits at boot: the image is not healthy. */
        ESP_LOGE(TAG, "no memory for the image check, rolling back");
        esp_err_t rc = esp_ota_mark_app_invalid_rollback_and_reboot();
        ESP_LOGE(TAG, "rollback failed: %s", esp_err_to_name(rc));
        s_image_check.last_error = "rollback failed";
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
    /* Rollback state of the running image: "pending_verify" after an OTA until
     * the image check passes (at the earliest after a minute), then "valid";
     * "untracked" after a USB flash. */
    esp_ota_img_states_t img_state;
    const char *img = "untracked";
    if (running && esp_ota_get_state_partition(running, &img_state) == ESP_OK) {
        img = img_state == ESP_OTA_IMG_VALID ? "valid"
            : img_state == ESP_OTA_IMG_PENDING_VERIFY ? "pending_verify"
            : img_state == ESP_OTA_IMG_NEW ? "new"
            : img_state == ESP_OTA_IMG_UNDEFINED ? "undefined" : "invalid";
    }
    cJSON_AddStringToObject(root, "image_state", img);
    if (s_image_check.started) {
        cJSON *chk = cJSON_AddObjectToObject(root, "image_check");
        cJSON_AddNumberToObject(chk, "checks", s_image_check.checks);
        if (s_image_check.last_error) cJSON_AddStringToObject(chk, "last_error", s_image_check.last_error);
        else cJSON_AddNullToObject(chk, "last_error");
        if (s_image_check.stack_free) cJSON_AddNumberToObject(chk, "stack_free", s_image_check.stack_free);
    }
    /* Set after the bootloader rolled back from a broken image. */
    if (s_rolled_back_from) cJSON_AddStringToObject(root, "rolled_back_from", s_rolled_back_from);
    else cJSON_AddNullToObject(root, "rolled_back_from");
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", esp_get_minimum_free_heap_size());
    /* TLS needs contiguous blocks; the total free heap alone can look fine
     * while no single allocation of the size mbedTLS asks for fits. */
    cJSON_AddNumberToObject(root, "largest_free_block", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    cJSON_AddBoolToObject(root, "mqtt_connected", amaran_mqtt_is_connected());
    cJSON_AddNumberToObject(root, "mqtt_connects", amaran_mqtt_connects());
    cJSON_AddBoolToObject(root, "mqtt_tls", amaran_mqtt_tls());

    /* This request is one of the sessions. */
    cJSON *sock = cJSON_AddObjectToObject(root, "sockets");
    cJSON_AddNumberToObject(sock, "open", sockets_open());
    cJSON_AddNumberToObject(sock, "max", CONFIG_LWIP_MAX_SOCKETS);
    cJSON_AddNumberToObject(sock, "http_sessions", http_sessions());
    cJSON_AddNumberToObject(sock, "http_sessions_max", HTTP_MAX_SESSIONS);

    cJSON *health = cJSON_AddObjectToObject(root, "health");
    cJSON_AddNumberToObject(health, "checks", s_health.checks);
    cJSON_AddNumberToObject(health, "fails_in_row", s_health.fails_in_row);
    cJSON_AddNumberToObject(health, "fails_to_restart", health_fails_to_restart());
    if (s_health.last_error) cJSON_AddStringToObject(health, "last_error", s_health.last_error);
    else cJSON_AddNullToObject(health, "last_error");
    if (s_health_task) cJSON_AddNumberToObject(health, "stack_free", uxTaskGetStackHighWaterMark(s_health_task));
    cJSON_AddNumberToObject(health, "restarts", s_health_rec.restarts);
    if (s_health_rec.restarts) {
        const health_record_t *r = &s_health_rec;
        cJSON *last = cJSON_AddObjectToObject(health, "last_restart");
        cJSON_AddNumberToObject(last, "uptime_s", r->uptime_s);
        char reason[sizeof(r->reason) + 1];
        memcpy(reason, r->reason, sizeof(r->reason));
        reason[sizeof(r->reason)] = '\0';
        cJSON_AddStringToObject(last, "reason", reason);
        if (r->err == CHECK_NOT_RUN) cJSON_AddNullToObject(last, "http_client");
        else cJSON_AddStringToObject(last, "http_client", esp_err_to_name(r->err));
        cJSON_AddNumberToObject(last, "errno", r->sock_errno);
        cJSON_AddNumberToObject(last, "http_status", r->http_status);
        cJSON_AddNumberToObject(last, "free_heap", r->free_heap);
        cJSON_AddNumberToObject(last, "min_free_heap", r->min_free_heap);
        cJSON_AddNumberToObject(last, "largest_free_block", r->largest_block);
        cJSON_AddNumberToObject(last, "sockets_open", r->sockets_open);
        cJSON_AddNumberToObject(last, "http_sessions", r->http_sessions);
        cJSON_AddNumberToObject(last, "stack_free", r->stack_free);
    }

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

/* Error answer once an authorised upload has begun. Returns ESP_FAIL like the
 * refusals: the server then closes the socket instead of reading the rest of a
 * ~1.7 MB body first. */
static esp_err_t ota_fail(httpd_req_t *req, const char *status, const char *error, int bytes)
{
    ota_trace(OTA_STAGE_FAILED, bytes);
    s_ota_busy = false;
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char body[64];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", error);
    httpd_resp_sendstr(req, body);
    return ESP_FAIL;
}

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

    ota_trace(OTA_STAGE_BEGIN, 0);
    s_ota_since_us = esp_timer_get_time();
    s_ota_gen++;
    s_ota_busy = true;   /* every failure below goes through ota_fail; success restarts */
    /* While the running image is still unconfirmed, esp_ota_begin refuses with
     * ESP_ERR_OTA_ROLLBACK_INVALID_STATE. Until 2026-09-14 that came back as a
     * 500 "ota begin" — after the whole body had been read. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK
        && state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "OTA refused: running image not confirmed yet");
        return ota_fail(req, "409 Conflict", "pending_verify", 0);
    }
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        return ota_fail(req, "500 Internal Server Error", "no ota partition", 0);
    }
    ESP_LOGI(TAG, "OTA to %s, expecting %d bytes", target->label, req->content_len);

    esp_ota_handle_t h = 0;
    esp_err_t rc = esp_ota_begin(target, OTA_SIZE_UNKNOWN, &h);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(rc));
        return ota_fail(req, "500 Internal Server Error", "ota begin", 0);
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
            esp_ota_abort(h);
            ESP_LOGE(TAG, "OTA aborted after %d bytes (recv=%d)", total, n);
            return ota_fail(req, "400 Bad Request", "recv", total);
        }
        if (esp_ota_write(h, buf, n) != ESP_OK) {
            esp_ota_abort(h);
            return ota_fail(req, "500 Internal Server Error", "write", total);
        }
        total += n; rest -= n;
        ota_trace(OTA_STAGE_RECEIVING, total);
    }
    ota_trace(OTA_STAGE_VERIFYING, total);

    /* esp_ota_end verifies the image's checksum and its signature. Only then may
     * the boot partition switch — otherwise the device would boot half an image,
     * or a foreign one. */
    if (esp_ota_end(h) != ESP_OK || esp_ota_set_boot_partition(target) != ESP_OK) {
        ESP_LOGE(TAG, "OTA image invalid, boot partition unchanged");
        return ota_fail(req, "400 Bad Request", "invalid image", total);
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
    cfg.max_open_sockets = HTTP_MAX_SESSIONS;   /* fits the socket pool, see top of file */
    /* A client that vanished without closing (a Mac going to sleep) keeps its
     * session until the purge; TCP keep-alive closes it after ~90 s instead. */
    cfg.keep_alive_enable = true;
    cfg.keep_alive_idle = 60;
    cfg.keep_alive_interval = 10;
    cfg.keep_alive_count = 3;
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
