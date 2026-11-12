/** @file furi_hal_bt_central.c
 * BLE central HAL facade: stack-gate, opaque-handle, dispatcher registration
 *
 * Every application callback (scan, connection, GATT) is delivered by
 * gap_central with the same connection pointer (&s_sentinel) and passes its
 * callback barrier. Init and deinit must not run concurrently with other calls
 * of this API.
 */

#include <furi_hal_bt_central.h>
#include <furi_hal_bt.h>
#include "../ble_glue/gap_central.h"
#include "../ble_glue/furi_ble/gatt_client.h"
#include "../ble_glue/furi_ble/event_dispatcher.h"
#include <furi.h>

#define TAG "FuriHalBtCentral"

/** Opaque connection sentinel - pointer identity is the "handle". */
struct FuriHalBtCentralConnection {
    uint8_t _reserved; /* non-zero-size struct requirement */
};

static struct FuriHalBtCentralConnection s_sentinel = {0};

static bool s_initialized = false;
static bool s_central_available = false;

/** Dispatcher handler registration token. */
static GapSvcEventHandler* s_handler = NULL;

/* Dispatcher callback (BLE event thread). The GATT client sees the event first;
 * gap_central then handles GAP events and consumes every leftover event that
 * carries a central connection handle, so none reaches the peripheral handlers. */
static BleEventAckStatus central_dispatcher_cb(void* pckt, void* ctx) {
    UNUSED(ctx);
    BleEventAckStatus status = gatt_client_handle_event(pckt);
    if(status != BleEventNotAck) {
        return status;
    }
    return gap_central_handle_event(pckt);
}

static inline bool central_usable(void) {
    return s_central_available && s_initialized && !gap_central_is_faulted();
}

/* ---------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------*/

void furi_hal_bt_central_init(void) {
    if(s_initialized) {
        FURI_LOG_W(TAG, "Already initialized");
        return;
    }

    /* Cache stack availability once - avoids repeated SHCI queries. */
    s_central_available = (furi_hal_bt_get_radio_stack() == FuriHalBtStackFull);
    if(!s_central_available) {
        FURI_LOG_I(TAG, "Light stack - central subsystem disabled");
        return;
    }

    /* gatt_client first: the central's deadline worker starts ticking it. */
    gatt_client_init();
    gap_central_init(&s_sentinel);

    s_handler = ble_event_dispatcher_register_svc_handler(central_dispatcher_cb, &s_sentinel);
    furi_check(s_handler);

    s_initialized = true;
    FURI_LOG_I(TAG, "Initialized");
}

void furi_hal_bt_central_deinit(void) {
    if(!s_initialized) {
        return;
    }

    /* Resolve scan, attempt and link (bounded), then the callback barrier. */
    gap_central_deinit();

    /* Waits until the BLE event thread is outside the handler list; after this
     * no event reaches the central code and the list is empty again for a
     * later profile reset. */
    ble_event_dispatcher_unregister_svc_handler(s_handler);
    s_handler = NULL;

    gatt_client_deinit();
    gap_central_free();

    s_initialized = false;
    s_central_available = false;

    FURI_LOG_I(TAG, "Deinitialized");
}

bool furi_hal_bt_central_available(void) {
    return central_usable();
}

bool furi_hal_bt_central_start_scan(FuriHalBtCentralScanCallback callback, void* context) {
    if(!central_usable() || !callback) {
        return false;
    }
    return gap_central_start_scan(callback, context);
}

bool furi_hal_bt_central_stop_scan(void) {
    if(!central_usable()) {
        return false;
    }
    return gap_central_stop_scan();
}

FuriHalBtCentralConnection* furi_hal_bt_central_connect(
    const uint8_t addr[FURI_HAL_BT_CENTRAL_ADDR_LEN],
    uint8_t addr_type,
    FuriHalBtCentralEventCallback callback,
    void* context) {
    if(!central_usable() || !addr) {
        return NULL;
    }
    if(!callback) {
        FURI_LOG_E(TAG, "connect: callback must be non-NULL");
        return NULL;
    }

    /* The callback is stored by gap_central only when the attempt is accepted,
     * so a rejected connect leaves the live link's callback untouched. */
    if(!gap_central_connect(addr, addr_type, callback, context)) {
        return NULL;
    }
    return &s_sentinel;
}

void furi_hal_bt_central_disconnect(FuriHalBtCentralConnection* conn) {
    if(conn != &s_sentinel) {
        FURI_LOG_W(TAG, "disconnect: invalid connection handle");
        return;
    }
    if(!central_usable()) {
        return;
    }
    gap_central_disconnect();
}

bool furi_hal_bt_central_gatt_discover(FuriHalBtCentralConnection* conn) {
    if(conn != &s_sentinel || !central_usable()) {
        return false;
    }
    return gatt_client_discover();
}

bool furi_hal_bt_central_gatt_discover_char(
    FuriHalBtCentralConnection* conn,
    const uint8_t uuid128[FURI_HAL_BT_CENTRAL_UUID128_LEN]) {
    if(conn != &s_sentinel || !central_usable() || !uuid128) {
        return false;
    }
    return gatt_client_discover_char(uuid128);
}

bool furi_hal_bt_central_gatt_write_nr(
    FuriHalBtCentralConnection* conn,
    uint16_t char_handle,
    const uint8_t* data,
    size_t len) {
    if(conn != &s_sentinel || !central_usable()) {
        return false;
    }
    return gatt_client_write_nr(char_handle, data, len);
}

bool furi_hal_bt_central_gatt_subscribe(FuriHalBtCentralConnection* conn, uint16_t char_handle) {
    if(conn != &s_sentinel || !central_usable()) {
        return false;
    }
    return gatt_client_subscribe(char_handle);
}

uint16_t furi_hal_bt_central_find_char(
    FuriHalBtCentralConnection* conn,
    const uint8_t uuid128[FURI_HAL_BT_CENTRAL_UUID128_LEN]) {
    if(conn != &s_sentinel || !central_usable() || !uuid128) {
        return 0;
    }
    return gatt_client_find_char(uuid128);
}
