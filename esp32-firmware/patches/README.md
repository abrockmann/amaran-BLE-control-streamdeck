# ESP-IDF patches

These patch the ESP-IDF BLE Mesh core. Both are **required**: 0001 for the
inbound status-snoop feature, 0002 so the mesh keeps working past the first
week. They live here because ESP-IDF is installed outside this repo
(`~/esp/esp-idf`), so the changes would otherwise be lost on a reinstall or
a different machine.

Tested against **ESP-IDF v5.3.2**.

## 0001-amaran-net-recv-status-snoop.patch

Adds a passive snoop in `components/bt/esp_ble_mesh/core/net.c`. Telink
fixtures route their `0x26` status replies to the provisioner unicast
`0x0001`, never to whoever requested status, so the normal access-layer
dispatch never delivers them to this node. The patch decrypts a *copy* of
the access payload in `bt_mesh_net_recv` (right after network decode,
before the relay path) and hands the plaintext to the firmware via the
weak hook `amaran_mesh_access_rx()`, which `main/main.c` implements. The
relay path is untouched, so ESP32->desktop refresh still works.

There is no public ESP-IDF callback for "raw decoded network PDU", so
patching net.c is unavoidable.

### Apply

```sh
cd ~/esp/esp-idf
git apply /path/to/esp32-firmware/patches/0001-amaran-net-recv-status-snoop.patch
git apply /path/to/esp32-firmware/patches/0002-amaran-adv-alloc-lock.patch
```

Then rebuild the firmware (`idf.py build`). To regenerate this patch after
further edits:

```sh
cd ~/esp/esp-idf
git diff -- components/bt/esp_ble_mesh/core/net.c \
  > /path/to/esp32-firmware/patches/0001-amaran-net-recv-status-snoop.patch
```

## 0002-amaran-adv-alloc-lock.patch

Backport of Espressif commit `87c0b28459` ("add adv lock to avoid allocated
buffers", `release/v5.3`, first shipped in **v5.3.5**) onto v5.3.2. Touches
`components/bt/esp_ble_mesh/core/adv.c` only.

**The bug.** `net_buf_alloc()` picks a buffer with `ref == 0` under the pool
lock but sets `ref = 1` and decrements the pool's free count *after* dropping
it. When two tasks allocate an advertising buffer at the same time (a
command from HTTP/MQTT landing while the 12 s status poll fires is enough)
both can claim the same buffer. The second `unref` is then swallowed by the
`!buf->ref` guard, and the pool's free count is one lower for good. The
count is what gates allocation, so after enough collisions the pool reports
empty while every buffer is actually free: mesh sends fail with `-ENOBUFS`
and the bridge goes deaf. On this bridge that took ~6-7 days (about 10
phantom losses per day). HTTP, heap and Wi-Fi all stay healthy, so the
health watchdog does not see it; only a reboot clears it.

**The fix** serialises `bt_mesh_adv_create_from_pool()` with a recursive
mutex so an allocation completes its bookkeeping before another one starts.

**Evidence.** Instrumented A/B on the same hardware and traffic: 43 phantom
losses in 93 h without the lock, 0 in 42 h with it. Running unconditionally
since 2026-10-04 with no recurrence (previously the bridge needed a reboot
roughly weekly).

If the build is ever moved to IDF **v5.3.5 or later this patch is already
upstream and must be dropped** (it will fail to apply). 0001 still applies.

To regenerate after further edits:

```sh
cd ~/esp/esp-idf
git diff -- components/bt/esp_ble_mesh/core/adv.c \
  > /path/to/esp32-firmware/patches/0002-amaran-adv-alloc-lock.patch
```
