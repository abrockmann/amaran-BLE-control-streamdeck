/*
 * Wi-Fi STA: connect to the SSID from wifi_config.h, retry on disconnect,
 * fire a callback once we have an IP. Coexists with BLE Mesh (which is
 * running on the same chip via the BTDM software coexist).
 */

#ifndef AMARAN_WIFI_H
#define AMARAN_WIFI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*amaran_wifi_got_ip_cb_t)(void);

int amaran_wifi_start(amaran_wifi_got_ip_cb_t got_ip_cb);
bool amaran_wifi_is_connected(void);
/* Wi-Fi reconnects since boot that got an IP address again (the first one
 * does not count). */
uint32_t amaran_wifi_reconnects(void);

#ifdef __cplusplus
}
#endif

#endif /* AMARAN_WIFI_H */
