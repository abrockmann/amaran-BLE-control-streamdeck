/*
 * MQTT client + Home Assistant discovery.
 *
 * On connect: publishes one HA MQTT-discovery message per fixture under
 *   <ha_prefix>/light/<device_id>_<key>/config
 * with brightness + CCT support, then subscribes to the per-fixture
 * command topic. Each incoming command is parsed as JSON and dispatched
 * to the mesh-send callback registered here.
 */

#ifndef AMARAN_MQTT_H
#define AMARAN_MQTT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mesh command dispatch — provided by main.c. The MQTT layer calls these
 * after parsing an incoming HA command payload. */
typedef struct {
    void (*on_off)(uint16_t dst, bool on);
    void (*brightness)(uint16_t dst, int pct_0_100);
    /* gm: green/magenta tint, -50..+50, 0 = neutral. */
    void (*cct)(uint16_t dst, int kelvin, int pct_0_100, int gm);
    void (*hsi)(uint16_t dst, int hue_0_360, int sat_0_100, int pct_0_100);
    void (*refresh)(uint16_t dst);
} amaran_mqtt_dispatch_t;

int amaran_mqtt_start(const amaran_mqtt_dispatch_t *dispatch);
bool amaran_mqtt_is_connected(void);
/* Successful MQTT connects since boot. Should stay close to
 * wifi.reconnects + 1; climbing by itself means the broker keeps dropping us. */
uint32_t amaran_mqtt_connects(void);
/* true if MQTT_URI uses mqtts:// */
bool amaran_mqtt_tls(void);

/* State of a light as the bridge knows it: fed by its own commands AND by the
 * status replies the core patch snoops. Without this accessor Stream Deck and
 * switch keys would have to guess the state — and guess wrong as soon as
 * someone else switches (HA, the app, the button on the fixture). */
typedef struct {
    bool on;
    int  brightness;   /* 0..100 */
    int  kelvin;
    int  hue;          /* 0..360 */
    int  sat;          /* 0..100 */
    int  gm;
    bool is_hs;        /* true: last set was color, false: color temperature */
} amaran_state_t;

bool amaran_mqtt_state_get(int idx, amaran_state_t *out);

/* Current on/off of the fixture at a mesh address. false if the address is
 * not a single known fixture (group, broadcast, unknown). */
bool amaran_mqtt_onoff_get(uint16_t addr, bool *on);

/* Record an on/off that was just sent from outside MQTT (HTTP). Without this
 * the state only catches up when the fixture's status reply is snooped, so a
 * toggle right after a switch would read the old value and HA would lag.
 * AMARAN_GROUP_ALL marks every fixture. */
void amaran_mqtt_note_onoff(uint16_t addr, bool on);

/* Report fixture state observed on the mesh (decoded from a 0x26 status
 * reply — e.g. a change made from the desktop / iOS app or a physical
 * knob). Updates HA's view only when a field differs materially from what
 * we last commanded, so the echo of our own commands doesn't churn HA.
 * brightness 0..100, cct in kelvin, gm -10..+10, hue 0..360, sat 0..100. */
void amaran_mqtt_report_state(uint16_t addr, bool on, bool is_hs,
                              int brightness_0_100, int cct_kelvin, int gm,
                              int hue_0_360, int sat_0_100);

#ifdef __cplusplus
}
#endif

#endif /* AMARAN_MQTT_H */
