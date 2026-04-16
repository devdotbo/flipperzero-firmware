/** @file furi_hal_bt_central.c
 * BLE central HAL facade: stack-gate, opaque-handle, dispatcher registration
 */

#include <furi_hal_bt_central.h>
#include <furi_hal_bt.h>
#include "../ble_glue/gap_central.h"
#include "../ble_glue/furi_ble/gatt_client.h"
#include "../ble_glue/furi_ble/event_dispatcher.h"
#include <furi.h>

#define TAG "FuriHalBtCentral"

/* ---------------------------------------------------------------------------
 * Module state
 * -------------------------------------------------------------------------*/

/** Opaque connection sentinel - pointer identity is the "handle". */
struct FuriHalBtCentralConnection {
    uint8_t _reserved; /* non-zero-size struct requirement */
};

static struct FuriHalBtCentralConnection s_sentinel = {0};

static bool s_initialized       = false;
static bool s_central_available = false;

/** User-supplied connection callback and context (single-slot). */
static FuriHalBtCentralEventCallback s_user_event_cb = NULL;
static void*                         s_user_ctx       = NULL;

/** Mutex protecting s_user_event_cb / s_user_ctx during reconnect. */
static FuriMutex* s_mutex = NULL;

/** Dispatcher handler registration token. */
static GapSvcEventHandler* s_handler = NULL;

/* ---------------------------------------------------------------------------
 * Facade-internal wrapper callback
 *
 * Registered with gap_central_connect as the "internal" callback.
 * When a connection completes, binds gatt_client to the real user cb.
 * When a disconnection occurs, detaches gatt_client.
 * Always forwards to the user callback.
 * -------------------------------------------------------------------------*/
static void facade_event_wrapper(
    FuriHalBtCentralConnection* conn,
    FuriHalBtCentralEvent       event,
    uint16_t                    char_handle,
    const uint8_t*              data,
    size_t                      len,
    void*                       context) {

    (void)context;
    (void)conn;

    if(event == FuriHalBtCentralEventConnected) {
        furi_mutex_acquire(s_mutex, FuriWaitForever);
        FuriHalBtCentralEventCallback cb  = s_user_event_cb;
        void*                         ctx = s_user_ctx;
        furi_mutex_release(s_mutex);

        gatt_client_attach(gap_central_get_conn_handle(), cb, ctx);
    }

    if(event == FuriHalBtCentralEventDisconnected) {
        gatt_client_attach(0, NULL, NULL);
    }

    furi_mutex_acquire(s_mutex, FuriWaitForever);
    FuriHalBtCentralEventCallback cb  = s_user_event_cb;
    void*                         ctx = s_user_ctx;
    furi_mutex_release(s_mutex);

    if(cb) {
        cb(&s_sentinel, event, char_handle, data, len, ctx);
    }
}

/* ---------------------------------------------------------------------------
 * Dispatcher callback - routes raw BLE packets to gap_central then
 * gatt_client; returns the first Ack, or the gatt_client result if gap
 * did not claim the event.
 * -------------------------------------------------------------------------*/
static BleEventAckStatus central_dispatcher_cb(void* pckt, void* ctx) {
    (void)ctx;
    BleEventAckStatus a = gap_central_handle_event(pckt);
    BleEventAckStatus b = gatt_client_handle_event(pckt);
    return (a != BleEventNotAck) ? a : b;
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

    s_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    furi_check(s_mutex);

    gap_central_init();
    gatt_client_init();

    s_handler = ble_event_dispatcher_register_svc_handler(central_dispatcher_cb, NULL);
    furi_check(s_handler);

    s_initialized = true;
    FURI_LOG_I(TAG, "Initialized");
}

void furi_hal_bt_central_deinit(void) {
    if(!s_initialized) {
        return;
    }

    /* Tear down any active link first. */
    if(gap_central_is_connected()) {
        gap_central_disconnect();
    }

    ble_event_dispatcher_unregister_svc_handler(s_handler);
    s_handler = NULL;

    gatt_client_deinit();
    gap_central_deinit();

    furi_mutex_free(s_mutex);
    s_mutex = NULL;

    s_user_event_cb     = NULL;
    s_user_ctx          = NULL;
    s_initialized       = false;
    s_central_available = false;

    FURI_LOG_I(TAG, "Deinitialized");
}

bool furi_hal_bt_central_available(void) {
    return s_central_available && s_initialized;
}

bool furi_hal_bt_central_start_scan(FuriHalBtCentralScanCallback callback, void* context) {
    if(!s_central_available || !s_initialized) {
        return false;
    }
    return gap_central_start_scan(callback, context);
}

bool furi_hal_bt_central_stop_scan(void) {
    if(!s_central_available || !s_initialized) {
        return false;
    }
    return gap_central_stop_scan();
}

FuriHalBtCentralConnection* furi_hal_bt_central_connect(
    const uint8_t                 addr[FURI_HAL_BT_CENTRAL_ADDR_LEN],
    uint8_t                       addr_type,
    FuriHalBtCentralEventCallback callback,
    void*                         context) {

    if(!s_central_available || !s_initialized) {
        return NULL;
    }
    if(!callback) {
        FURI_LOG_E(TAG, "connect: callback must be non-NULL");
        return NULL;
    }

    /* Store user callback before calling gap_central_connect so that the
     * wrapper can forward events the moment the connection completes. */
    furi_mutex_acquire(s_mutex, FuriWaitForever);
    s_user_event_cb = callback;
    s_user_ctx      = context;
    furi_mutex_release(s_mutex);

    bool ok = gap_central_connect(addr, addr_type, facade_event_wrapper, NULL);
    if(!ok) {
        FURI_LOG_E(TAG, "connect: gap_central_connect failed");
        furi_mutex_acquire(s_mutex, FuriWaitForever);
        s_user_event_cb = NULL;
        s_user_ctx      = NULL;
        furi_mutex_release(s_mutex);
        return NULL;
    }

    return &s_sentinel;
}

void furi_hal_bt_central_disconnect(FuriHalBtCentralConnection* conn) {
    if(conn != &s_sentinel) {
        FURI_LOG_W(TAG, "disconnect: invalid connection handle");
        return;
    }
    if(!s_central_available || !s_initialized) {
        return;
    }
    gap_central_disconnect();
}

bool furi_hal_bt_central_gatt_discover(FuriHalBtCentralConnection* conn) {
    if(conn != &s_sentinel) {
        return false;
    }
    if(!s_central_available || !s_initialized) {
        return false;
    }
    return gatt_client_discover();
}

bool furi_hal_bt_central_gatt_write_nr(
    FuriHalBtCentralConnection* conn,
    uint16_t                    char_handle,
    const uint8_t*              data,
    size_t                      len) {

    if(conn != &s_sentinel) {
        return false;
    }
    if(!s_central_available || !s_initialized) {
        return false;
    }
    return gatt_client_write_nr(char_handle, data, len);
}

bool furi_hal_bt_central_gatt_subscribe(
    FuriHalBtCentralConnection* conn,
    uint16_t                    char_handle) {

    if(conn != &s_sentinel) {
        return false;
    }
    if(!s_central_available || !s_initialized) {
        return false;
    }
    return gatt_client_subscribe(char_handle);
}

uint16_t furi_hal_bt_central_find_char(
    FuriHalBtCentralConnection*                conn,
    const uint8_t uuid128[FURI_HAL_BT_CENTRAL_UUID128_LEN]) {

    if(conn != &s_sentinel) {
        return 0;
    }
    if(!s_central_available || !s_initialized) {
        return 0;
    }
    return gatt_client_find_char(uuid128);
}
