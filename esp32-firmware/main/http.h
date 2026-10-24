/*
 * Tiny HTTP REST API mirroring the UART CLI:
 *   GET  /lights
 *   GET  /status                    Wi-Fi signal, Bluetooth reach, on/off rounds
 *   POST /lights/<key>/on
 *   POST /lights/<key>/off
 *   POST /lights/<key>/toggle
 *   POST /lights/<key>/brightness   { "value": 0-100 }
 *   POST /lights/<key>/cct          { "kelvin": int, "intensity": 0-100, "gm": -50..50 }
 *   POST /lights/<key>/hsi          { "hue": 0-360, "sat": 0-100, "intensity": 0-100 }
 *
 * <key> may also be "all" / "broadcast" for group sends.
 */

#ifndef AMARAN_HTTP_H
#define AMARAN_HTTP_H

#include "mqtt.h"  /* reuse the same dispatch struct */

#ifdef __cplusplus
extern "C" {
#endif

int amaran_http_start(const amaran_mqtt_dispatch_t *dispatch);

/* Call once at boot. Notes whether the bootloader rolled back, and for an image
 * that is still pending_verify starts the check that confirms it — or rolls it
 * back if it hasn't passed within 5 minutes. After that (and right away for a
 * confirmed image) the same check runs once a minute as a watchdog: three
 * failures in a row restart the bridge, recorded in GET /status. */
void amaran_image_check_start(void);

/* Diagnostics for GET /status, kept by main.c. They exist so the bridge's
 * placement can be judged without a USB cable: Wi-Fi signal comes from the
 * driver, Bluetooth reach from these counters. */
#define AMARAN_ONOFF_ROUNDS 3

typedef struct {
    uint32_t asked;         /* status requests sent since boot */
    uint32_t heard;         /* status replies decoded since boot */
    int32_t  heard_s_ago;   /* seconds since the last reply, -1 = never */
} amaran_fixture_diag_t;

typedef struct {
    uint32_t confirmed_in_round[AMARAN_ONOFF_ROUNDS];
    uint32_t not_confirmed;
} amaran_onoff_diag_t;

void amaran_node_fixture_diag(int idx, amaran_fixture_diag_t *out);
void amaran_node_onoff_diag(amaran_onoff_diag_t *out);

/* The mesh's own health, for GET /status and the watchdog. The BLE Mesh stack
 * sends every PDU from a fixed pool of advertising buffers (adv_buf_pool in
 * IDF's core/adv.c, CONFIG_BLE_MESH_ADV_BUF_COUNT of them). A send with no
 * buffer left fails with -ENOBUFS while HTTP, heap and Wi-Fi look fine: the
 * mesh is deaf and nothing else notices. adv_free is the pool's own count of
 * free buffers; adv_idle counts the buffers whose reference count is 0, read
 * directly. The two agree on a healthy bridge. A buffer that is idle but not
 * counted free is a "ghost": the bookkeeping lost it (the IDF v5.3.2 alloc
 * race that patches/0002 fixes, or the smaller gap that remains in
 * net_buf_unref / net_buf_alloc_len). ghosts > 0 means the race is back;
 * adv_free reaching 0 means the mesh is deaf, whether to ghosts or to buffers
 * truly held. Reads are unlocked, so one sample can be off by one. */
typedef struct {
    int16_t  adv_total;     /* buffers in the pool */
    int16_t  adv_free;      /* the pool's free count (what allocation trusts) */
    int16_t  adv_idle;      /* buffers with ref == 0, counted directly */
    int16_t  adv_ghosts;    /* adv_idle - adv_free; 0 on a healthy bridge */
    uint32_t send_errors;   /* model sends that completed with an error, since boot */
    uint32_t send_enobufs;  /* of which -ENOBUFS (pool empty) */
    int32_t  send_err_last; /* the last error code, 0 = none yet */
} amaran_mesh_diag_t;

void amaran_node_mesh_diag(amaran_mesh_diag_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AMARAN_HTTP_H */
