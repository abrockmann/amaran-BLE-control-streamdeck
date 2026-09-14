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
 * back if it hasn't passed within 5 minutes. */
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

#ifdef __cplusplus
}
#endif

#endif /* AMARAN_HTTP_H */
