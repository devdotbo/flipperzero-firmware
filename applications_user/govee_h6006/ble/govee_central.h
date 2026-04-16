#pragma once

#include <furi.h>
#include <furi_hal_bt_central.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct GoveeCentral GoveeCentral;

typedef enum {
    GoveeCentralEventScanResult,
    GoveeCentralEventScanComplete,
    GoveeCentralEventConnected,
    GoveeCentralEventDisconnected,
    GoveeCentralEventDiscoveryComplete,
    GoveeCentralEventWriteComplete,
    GoveeCentralEventError,
} GoveeCentralEvent;

typedef struct {
    uint8_t addr_type;
    uint8_t addr[6];
    int8_t rssi;
    char name[32];
} GoveeCentralScanEntry;

typedef void (*GoveeCentralEventCallback)(
    GoveeCentral* gc,
    GoveeCentralEvent event,
    const void* data,
    void* context);

GoveeCentral* govee_central_alloc(void);
void govee_central_free(GoveeCentral* gc);

void govee_central_set_callback(
    GoveeCentral* gc,
    GoveeCentralEventCallback cb,
    void* context);

bool govee_central_available(void);

bool govee_central_scan_start(GoveeCentral* gc);
bool govee_central_scan_stop(GoveeCentral* gc);

bool govee_central_connect(
    GoveeCentral* gc,
    const uint8_t addr[6],
    uint8_t addr_type);
void govee_central_disconnect(GoveeCentral* gc);
bool govee_central_is_connected(GoveeCentral* gc);

bool govee_central_send_power(GoveeCentral* gc, bool on);
bool govee_central_send_brightness(GoveeCentral* gc, uint8_t brightness);
bool govee_central_send_rgb(GoveeCentral* gc, uint8_t r, uint8_t g, uint8_t b);
bool govee_central_send_ct(GoveeCentral* gc, uint16_t kelvin);

void govee_central_keepalive_start(GoveeCentral* gc, uint32_t interval_ms);
void govee_central_keepalive_stop(GoveeCentral* gc);
