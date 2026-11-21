#pragma once
// NimBLE peripheral that presents itself to an LG webOS TV as a Magic Remote.
// GATT layout and values follow the MR25GB notes in github.com/wittenbe/magic-remote-bridge (docs/protocol.md);
// everything the TV reads, writes or subscribes to is logged, because the 2019 TV may expect other values.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum lgmr_report { LGMR_F9_IN = 0, LGMR_FD_IN = 1, LGMR_FE_IN = 2 };

// Called from the NimBLE host task.
typedef void (*lgmr_write_cb_t)(const char *what, const uint8_t *data, uint16_t len);
typedef void (*lgmr_subscribed_cb_t)(int report, bool on);

// Implemented on the C++ side: forwards to the ESPHome logger (level uses ESPHome's numbering).
void lgmr_logf(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Before lgmr_init: Bluetooth public address, MSB first as printed.
void lgmr_set_address(const uint8_t addr[6]);
void lgmr_init(const char *name, const uint8_t *mfg, uint8_t mfg_len, lgmr_write_cb_t on_write,
               lgmr_subscribed_cb_t on_sub);
bool lgmr_synced(void);
void lgmr_set_identity(const char *name, const uint8_t *mfg, uint8_t mfg_len);
void lgmr_set_layout(bool hid, bool name_main, const uint8_t *mfg, uint8_t mfg_len);
void lgmr_set_flags(uint8_t flags, const uint8_t *mfg, uint8_t mfg_len);
void lgmr_advertise(bool on);
// While bonded and disconnected: a directed advertising burst to the TV, then back to undirected.
void lgmr_reconnect(void);
// Called from the main loop: deferred security handling.
void lgmr_poll(void);
bool lgmr_connected(void);
bool lgmr_subscribed(int report);
int lgmr_notify(int report, const uint8_t *data, uint16_t len);
void lgmr_status(void);
void lgmr_forget(void);
// Wake-over-Bluetooth burst for a TV in standby (see lgmr_ble.c). 0 on success; LGMR_WOBLE_CONNECTED while the TV
// is connected (so it is on), another nonzero NimBLE code on failure.
#define LGMR_WOBLE_CONNECTED 1000
int lgmr_woble_burst(const uint8_t id[6], uint32_t ms, int pdu);
// Same, with any advertising data (at most 31 bytes), for trying wake packet variants.
int lgmr_adv_burst(const uint8_t *data, size_t len, uint32_t ms, int pdu);
void lgmr_disconnect(void);
uint8_t lgmr_own_addr_type(void);
// The Bluetooth address in use, most significant byte first (valid once the host has synced).
void lgmr_own_address(uint8_t out[6]);
const char *lgmr_device_name(void);

// Capture mode (lgmr_capture.c): the C6 as a TV for a real Magic Remote; everything it sends is logged.
void lgmr_cap_start(void);
void lgmr_cap_stop(void);
void lgmr_cap_forget(void);
void lgmr_cap_dump(void);
bool lgmr_cap_active(void);
int lgmr_cap_cmd(const uint8_t *d, uint16_t n);  // F9 command to the remote's output report
void lgmr_cap_poll(void);                        // from the main loop

#ifdef __cplusplus
}
#endif
