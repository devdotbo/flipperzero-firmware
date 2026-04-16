#include "govee_central.h"
#include "../protocol/govee_h6006.h"

#include <furi.h>
#include <furi_hal_bt_central.h>
#include <string.h>
#include <stdlib.h>

#define TAG "GoveeCentral"

#define GOVEE_FILTER_PREFIX "ihoment_H6006"

/* Parse GOVEE_SERVICE_UUID / GOVEE_CHAR_WRITE_UUID hex strings to byte arrays. */
static void uuid_hex_to_bytes(const char* hex, uint8_t out[16]) {
    for(int i = 0; i < 16; i++) {
        uint8_t hi = hex[i * 2];
        uint8_t lo = hex[i * 2 + 1];
        hi = (hi >= 'a') ? (hi - 'a' + 10) : (hi >= 'A') ? (hi - 'A' + 10) : (hi - '0');
        lo = (lo >= 'a') ? (lo - 'a' + 10) : (lo >= 'A') ? (lo - 'A' + 10) : (lo - '0');
        out[i] = (hi << 4) | lo;
    }
}

struct GoveeCentral {
    FuriMutex* mutex;

    /* User callback */
    GoveeCentralEventCallback callback;
    void* callback_context;

    /* Connection state */
    FuriHalBtCentralConnection* conn;
    bool scanning;
    bool discovering; /* discovery in-flight, not yet exposed to user */

    /* Cached characteristic handle (valid after discovery) */
    uint16_t write_char_handle;

    /* Pre-allocated packet buffer - no heap in callbacks */
    uint8_t packet[GOVEE_PACKET_SIZE];

    /* Keepalive timer */
    FuriTimer* keepalive_timer;
};

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

static void emit_event(GoveeCentral* gc, GoveeCentralEvent event, const void* data) {
    if(gc->callback) {
        gc->callback(gc, event, data, gc->callback_context);
    }
}

/* Parse advertising data (AD structures) looking for a local name record
 * (type 0x08 or 0x09).  Returns true and fills name_out (NUL-terminated,
 * up to name_max-1 chars) when a matching name is found. */
static bool adv_data_get_name(
    const uint8_t* data,
    uint8_t data_len,
    char* name_out,
    size_t name_max) {
    uint8_t i = 0;
    while(i < data_len) {
        uint8_t len = data[i];
        if(len == 0) break;
        if(i + len >= data_len) break; /* malformed */

        uint8_t type = data[i + 1];
        if(type == 0x08 || type == 0x09) {
            uint8_t name_len = len - 1; /* subtract type byte */
            if(name_len >= name_max) name_len = name_max - 1;
            memcpy(name_out, &data[i + 2], name_len);
            name_out[name_len] = '\0';
            return true;
        }
        i += len + 1;
    }
    return false;
}

/* --------------------------------------------------------------------------
 * HAL scan callback
 * -------------------------------------------------------------------------- */

static void hal_scan_callback(const FuriHalBtCentralScanResult* result, void* context) {
    GoveeCentral* gc = context;
    furi_assert(gc);

    char name[32] = {0};
    if(!adv_data_get_name(result->data, result->data_len, name, sizeof(name))) {
        return; /* no name record - skip */
    }

    if(strncmp(name, GOVEE_FILTER_PREFIX, strlen(GOVEE_FILTER_PREFIX)) != 0) {
        return; /* not an H6006 */
    }

    FURI_LOG_I(TAG, "Found device: %s  RSSI=%d", name, result->rssi);

    GoveeCentralScanEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.addr_type = result->addr_type;
    memcpy(entry.addr, result->addr, 6);
    entry.rssi = result->rssi;
    strncpy(entry.name, name, sizeof(entry.name) - 1);

    emit_event(gc, GoveeCentralEventScanResult, &entry);
}

/* --------------------------------------------------------------------------
 * HAL connection callback
 * -------------------------------------------------------------------------- */

static void hal_conn_callback(
    FuriHalBtCentralConnection* conn,
    FuriHalBtCentralEvent event,
    uint16_t char_handle,
    const uint8_t* data,
    size_t len,
    void* context) {
    UNUSED(conn);
    UNUSED(char_handle);
    UNUSED(data);
    UNUSED(len);

    GoveeCentral* gc = context;
    furi_assert(gc);

    switch(event) {
    case FuriHalBtCentralEventConnected:
        FURI_LOG_I(TAG, "HAL connected - starting discovery");
        gc->discovering = true;
        if(!furi_hal_bt_central_gatt_discover(gc->conn)) {
            FURI_LOG_E(TAG, "gatt_discover failed");
            gc->discovering = false;
            emit_event(gc, GoveeCentralEventError, NULL);
        }
        break;

    case FuriHalBtCentralEventDiscoveryComplete: {
        FURI_LOG_I(TAG, "Discovery complete - resolving write char");
        uint8_t uuid_bytes[16];
        uuid_hex_to_bytes(GOVEE_CHAR_WRITE_UUID, uuid_bytes);
        uint16_t handle = furi_hal_bt_central_find_char(gc->conn, uuid_bytes);
        if(handle == 0) {
            FURI_LOG_E(TAG, "Write characteristic not found");
            gc->discovering = false;
            emit_event(gc, GoveeCentralEventError, NULL);
        } else {
            FURI_LOG_I(TAG, "Write char handle=0x%04x", handle);
            gc->write_char_handle = handle;
            gc->discovering = false;
            emit_event(gc, GoveeCentralEventConnected, NULL);
        }
        break;
    }

    case FuriHalBtCentralEventWriteComplete:
        emit_event(gc, GoveeCentralEventWriteComplete, NULL);
        break;

    case FuriHalBtCentralEventDisconnected:
        FURI_LOG_I(TAG, "Disconnected");
        gc->conn = NULL;
        gc->write_char_handle = 0;
        gc->discovering = false;
        emit_event(gc, GoveeCentralEventDisconnected, NULL);
        break;

    case FuriHalBtCentralEventNotification:
        /* Not subscribed to notifications - ignore */
        break;

    case FuriHalBtCentralEventError:
        FURI_LOG_E(TAG, "HAL connection error");
        gc->conn = NULL;
        gc->write_char_handle = 0;
        gc->discovering = false;
        emit_event(gc, GoveeCentralEventError, NULL);
        break;

    default:
        break;
    }
}

/* --------------------------------------------------------------------------
 * Keepalive timer callback
 * -------------------------------------------------------------------------- */

static void keepalive_timer_callback(void* context) {
    GoveeCentral* gc = context;
    furi_assert(gc);

    if(!gc->conn || gc->write_char_handle == 0) return;

    govee_h6006_create_keepalive_packet(gc->packet);
    furi_hal_bt_central_gatt_write_nr(
        gc->conn, gc->write_char_handle, gc->packet, GOVEE_PACKET_SIZE);
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

GoveeCentral* govee_central_alloc(void) {
    GoveeCentral* gc = malloc(sizeof(GoveeCentral));
    furi_assert(gc);
    memset(gc, 0, sizeof(GoveeCentral));

    gc->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    furi_assert(gc->mutex);

    gc->keepalive_timer =
        furi_timer_alloc(keepalive_timer_callback, FuriTimerTypePeriodic, gc);
    furi_assert(gc->keepalive_timer);

    FURI_LOG_I(TAG, "alloc");
    return gc;
}

void govee_central_free(GoveeCentral* gc) {
    furi_assert(gc);

    govee_central_keepalive_stop(gc);
    furi_timer_free(gc->keepalive_timer);

    if(gc->scanning) {
        furi_hal_bt_central_stop_scan();
        gc->scanning = false;
    }

    if(gc->conn) {
        furi_hal_bt_central_disconnect(gc->conn);
        /* conn pointer cleared in callback, but we won't receive it now */
        gc->conn = NULL;
    }

    furi_mutex_free(gc->mutex);
    free(gc);
    FURI_LOG_I(TAG, "free");
}

void govee_central_set_callback(
    GoveeCentral* gc,
    GoveeCentralEventCallback cb,
    void* context) {
    furi_assert(gc);
    furi_mutex_acquire(gc->mutex, FuriWaitForever);
    gc->callback = cb;
    gc->callback_context = context;
    furi_mutex_release(gc->mutex);
}

bool govee_central_available(void) {
    return furi_hal_bt_central_available();
}

bool govee_central_scan_start(GoveeCentral* gc) {
    furi_assert(gc);
    if(gc->scanning) return true;

    bool ok = furi_hal_bt_central_start_scan(hal_scan_callback, gc);
    if(ok) {
        gc->scanning = true;
        FURI_LOG_I(TAG, "scan started");
    } else {
        FURI_LOG_E(TAG, "scan start failed");
    }
    return ok;
}

bool govee_central_scan_stop(GoveeCentral* gc) {
    furi_assert(gc);
    if(!gc->scanning) return true;

    bool ok = furi_hal_bt_central_stop_scan();
    gc->scanning = false;
    FURI_LOG_I(TAG, "scan stopped");
    return ok;
}

bool govee_central_connect(GoveeCentral* gc, const uint8_t addr[6], uint8_t addr_type) {
    furi_assert(gc);

    if(gc->conn) {
        FURI_LOG_E(TAG, "already connected");
        return false;
    }

    /* Stop scan before connecting (HAL does it too, but be explicit) */
    if(gc->scanning) {
        furi_hal_bt_central_stop_scan();
        gc->scanning = false;
    }

    gc->conn = furi_hal_bt_central_connect(addr, addr_type, hal_conn_callback, gc);
    if(!gc->conn) {
        FURI_LOG_E(TAG, "connect failed");
        return false;
    }

    FURI_LOG_I(TAG, "connect initiated");
    return true;
}

void govee_central_disconnect(GoveeCentral* gc) {
    furi_assert(gc);
    if(!gc->conn) return;
    furi_hal_bt_central_disconnect(gc->conn);
    /* State cleared in HAL callback */
}

bool govee_central_is_connected(GoveeCentral* gc) {
    furi_assert(gc);
    return gc->conn != NULL && gc->write_char_handle != 0;
}

/* --------------------------------------------------------------------------
 * Send helpers
 * -------------------------------------------------------------------------- */

bool govee_central_send_power(GoveeCentral* gc, bool on) {
    furi_assert(gc);
    if(!govee_central_is_connected(gc)) return false;

    govee_h6006_create_power_packet(gc->packet, on);
    return furi_hal_bt_central_gatt_write_nr(
        gc->conn, gc->write_char_handle, gc->packet, GOVEE_PACKET_SIZE);
}

bool govee_central_send_brightness(GoveeCentral* gc, uint8_t brightness) {
    furi_assert(gc);
    if(!govee_central_is_connected(gc)) return false;

    govee_h6006_create_brightness_packet(gc->packet, brightness);
    return furi_hal_bt_central_gatt_write_nr(
        gc->conn, gc->write_char_handle, gc->packet, GOVEE_PACKET_SIZE);
}

bool govee_central_send_rgb(GoveeCentral* gc, uint8_t r, uint8_t g, uint8_t b) {
    furi_assert(gc);
    if(!govee_central_is_connected(gc)) return false;

    govee_h6006_create_color_packet(gc->packet, r, g, b);
    return furi_hal_bt_central_gatt_write_nr(
        gc->conn, gc->write_char_handle, gc->packet, GOVEE_PACKET_SIZE);
}

bool govee_central_send_ct(GoveeCentral* gc, uint16_t kelvin) {
    furi_assert(gc);
    if(!govee_central_is_connected(gc)) return false;

    govee_h6006_create_white_packet(gc->packet, kelvin);
    return furi_hal_bt_central_gatt_write_nr(
        gc->conn, gc->write_char_handle, gc->packet, GOVEE_PACKET_SIZE);
}

/* --------------------------------------------------------------------------
 * Keepalive
 * -------------------------------------------------------------------------- */

void govee_central_keepalive_start(GoveeCentral* gc, uint32_t interval_ms) {
    furi_assert(gc);
    furi_assert(interval_ms > 0);
    furi_timer_start(gc->keepalive_timer, interval_ms);
    FURI_LOG_I(TAG, "keepalive started interval=%lu ms", (unsigned long)interval_ms);
}

void govee_central_keepalive_stop(GoveeCentral* gc) {
    furi_assert(gc);
    furi_timer_stop(gc->keepalive_timer);
    FURI_LOG_I(TAG, "keepalive stopped");
}
