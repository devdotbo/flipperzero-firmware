/** @file gap_central.c
 * BLE central GAP: scan + connect/disconnect + event dispatch
 */

#include "gap_central.h"

#include "app_common.h"
#include <core/mutex.h>
#include <ble/ble.h>

#include <furi_hal.h>
#include <furi_hal_bt.h>
#include <furi.h>

#include <stdint.h>
#include <string.h>

#define TAG "BleGapCentral"

#define GAP_CENTRAL_SCAN_INTERVAL     (0x0320U) /* 500 ms */
#define GAP_CENTRAL_SCAN_WINDOW       (0x0320U) /* 500 ms */
#define GAP_CENTRAL_CONN_INTERVAL_MIN (40U) /* 50 ms */
#define GAP_CENTRAL_CONN_INTERVAL_MAX (80U) /* 100 ms */
#define GAP_CENTRAL_CONN_LATENCY      (0U)
#define GAP_CENTRAL_SUPERVISION_TMO   (0x01F4U) /* 5000 ms */
#define GAP_CENTRAL_CE_MIN            (16U)
#define GAP_CENTRAL_CE_MAX            (16U)

#define GAP_CENTRAL_DISCONNECT_REASON (0x13U) /* Remote user terminated */

#define GAP_CENTRAL_SCAN_DEDUP_SLOTS  (32U)

/* Implemented by sibling file gatt_client.c (Phase 2.3). Called on disconnect
 * so GATT bookkeeping is cleared before we announce FuriHalBtCentralEventDisconnected
 * to the user.
 */
extern void gatt_client_reset(void);

typedef enum {
    GapCentralStateIdle,
    GapCentralStateScanning,
    GapCentralStateConnecting,
    GapCentralStateConnected,
} GapCentralState;

typedef struct {
    bool occupied;
    uint8_t addr[6];
} GapCentralSeenPeer;

typedef struct {
    GapCentralState state;
    FuriMutex* state_mutex;

    /* Scan */
    FuriHalBtCentralScanCallback scan_cb;
    void* scan_ctx;
    GapCentralSeenPeer seen[GAP_CENTRAL_SCAN_DEDUP_SLOTS];
    uint8_t seen_count;

    /* Connection */
    uint16_t conn_handle;
    uint8_t peer_addr[6];
    uint8_t peer_addr_type;
    FuriHalBtCentralEventCallback event_cb;
    void* event_ctx;

    bool initialized;
} GapCentral;

static GapCentral gap_central;

/* --- small helpers ------------------------------------------------------- */

static inline void gap_central_lock(void) {
    furi_check(furi_mutex_acquire(gap_central.state_mutex, FuriWaitForever) == FuriStatusOk);
}

static inline void gap_central_unlock(void) {
    furi_check(furi_mutex_release(gap_central.state_mutex) == FuriStatusOk);
}

static void gap_central_scan_ring_reset(void) {
    memset(gap_central.seen, 0, sizeof(gap_central.seen));
    gap_central.seen_count = 0;
}

/* Returns true when the peer is new (and was inserted into the ring).
 * Ring is write-only: once full it silently drops new peers until next scan.
 */
static bool gap_central_scan_ring_mark(const uint8_t addr[6]) {
    for(uint8_t i = 0; i < GAP_CENTRAL_SCAN_DEDUP_SLOTS; i++) {
        if(gap_central.seen[i].occupied && memcmp(gap_central.seen[i].addr, addr, 6) == 0) {
            return false;
        }
    }
    if(gap_central.seen_count < GAP_CENTRAL_SCAN_DEDUP_SLOTS) {
        GapCentralSeenPeer* slot = &gap_central.seen[gap_central.seen_count++];
        slot->occupied = true;
        memcpy(slot->addr, addr, 6);
    }
    return true;
}

/* --- init / deinit ------------------------------------------------------- */

void gap_central_init(void) {
    furi_check(!gap_central.initialized);

    memset(&gap_central, 0, sizeof(gap_central));
    gap_central.state_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    gap_central.state = GapCentralStateIdle;
    gap_central.conn_handle = 0xFFFF;
    gap_central.initialized = true;

    FURI_LOG_I(TAG, "Init");
}

void gap_central_deinit(void) {
    if(!gap_central.initialized) {
        return;
    }

    /* Best-effort shutdown: if anything is in flight, tear it down. */
    (void)gap_central_disconnect();
    (void)gap_central_stop_scan();

    furi_mutex_free(gap_central.state_mutex);
    memset(&gap_central, 0, sizeof(gap_central));

    FURI_LOG_I(TAG, "Deinit");
}

/* --- scan ---------------------------------------------------------------- */

bool gap_central_start_scan(FuriHalBtCentralScanCallback cb, void* ctx) {
    furi_check(cb);
    furi_check(gap_central.initialized);

    gap_central_lock();
    if(gap_central.state != GapCentralStateIdle) {
        FURI_LOG_E(TAG, "start_scan: bad state %d", gap_central.state);
        gap_central_unlock();
        return false;
    }
    gap_central.scan_cb = cb;
    gap_central.scan_ctx = ctx;
    gap_central_scan_ring_reset();
    gap_central.state = GapCentralStateScanning;
    gap_central_unlock();

    furi_hal_bt_lock_core2();
    tBleStatus status = aci_gap_start_general_discovery_proc(
        GAP_CENTRAL_SCAN_INTERVAL, GAP_CENTRAL_SCAN_WINDOW, GAP_PUBLIC_ADDR, 1);
    furi_hal_bt_unlock_core2();

    if(status) {
        FURI_LOG_E(TAG, "start_general_discovery_proc failed %x", status);
        gap_central_lock();
        gap_central.state = GapCentralStateIdle;
        gap_central.scan_cb = NULL;
        gap_central.scan_ctx = NULL;
        gap_central_unlock();
        return false;
    }

    FURI_LOG_I(TAG, "Scan started");
    return true;
}

bool gap_central_stop_scan(void) {
    furi_check(gap_central.initialized);

    gap_central_lock();
    if(gap_central.state != GapCentralStateScanning) {
        /* Idempotent: nothing to stop is fine. */
        gap_central_unlock();
        return true;
    }
    gap_central_unlock();

    furi_hal_bt_lock_core2();
    tBleStatus status = aci_gap_terminate_gap_proc(GAP_GENERAL_DISCOVERY_PROC);
    furi_hal_bt_unlock_core2();

    if(status) {
        FURI_LOG_E(TAG, "terminate_gap_proc failed %x", status);
        return false;
    }

    /* State transitions to Idle when ACI_GAP_PROC_COMPLETE_VSEVT_CODE arrives.
     * Do not flip here; the completion event is the source of truth.
     */
    FURI_LOG_I(TAG, "Scan stop requested");
    return true;
}

/* --- connect / disconnect ----------------------------------------------- */

bool gap_central_connect(
    const uint8_t addr[6],
    uint8_t addr_type,
    FuriHalBtCentralEventCallback cb,
    void* ctx) {
    furi_check(addr);
    furi_check(cb);
    furi_check(gap_central.initialized);

    gap_central_lock();
    if(gap_central.state != GapCentralStateIdle) {
        FURI_LOG_E(TAG, "connect: bad state %d", gap_central.state);
        gap_central_unlock();
        return false;
    }
    memcpy(gap_central.peer_addr, addr, 6);
    gap_central.peer_addr_type = addr_type;
    gap_central.event_cb = cb;
    gap_central.event_ctx = ctx;
    gap_central.state = GapCentralStateConnecting;
    gap_central_unlock();

    furi_hal_bt_lock_core2();
    tBleStatus status = aci_gap_create_connection(
        GAP_CENTRAL_SCAN_INTERVAL,
        GAP_CENTRAL_SCAN_WINDOW,
        addr_type,
        addr,
        GAP_PUBLIC_ADDR,
        GAP_CENTRAL_CONN_INTERVAL_MIN,
        GAP_CENTRAL_CONN_INTERVAL_MAX,
        GAP_CENTRAL_CONN_LATENCY,
        GAP_CENTRAL_SUPERVISION_TMO,
        GAP_CENTRAL_CE_MIN,
        GAP_CENTRAL_CE_MAX);
    furi_hal_bt_unlock_core2();

    if(status) {
        FURI_LOG_E(TAG, "create_connection failed %x", status);
        gap_central_lock();
        gap_central.state = GapCentralStateIdle;
        gap_central.event_cb = NULL;
        gap_central.event_ctx = NULL;
        gap_central_unlock();
        return false;
    }

    FURI_LOG_I(TAG, "Connect requested");
    return true;
}

bool gap_central_disconnect(void) {
    furi_check(gap_central.initialized);

    gap_central_lock();
    bool connected = (gap_central.state == GapCentralStateConnected) ||
                     (gap_central.state == GapCentralStateConnecting);
    uint16_t handle = gap_central.conn_handle;
    gap_central_unlock();

    if(!connected) {
        return true;
    }

    if(handle == 0xFFFF) {
        /* Connecting but handle not yet assigned: cannot cleanly abort mid-air.
         * The firmware will fail the conn attempt and deliver an error event;
         * treat this as a no-op success for the caller.
         */
        return true;
    }

    furi_hal_bt_lock_core2();
    tBleStatus status = aci_gap_terminate(handle, GAP_CENTRAL_DISCONNECT_REASON);
    furi_hal_bt_unlock_core2();

    if(status) {
        FURI_LOG_E(TAG, "terminate failed %x", status);
        return false;
    }

    FURI_LOG_I(TAG, "Disconnect requested");
    return true;
}

bool gap_central_is_connected(void) {
    if(!gap_central.initialized) {
        return false;
    }
    gap_central_lock();
    bool r = (gap_central.state == GapCentralStateConnected);
    gap_central_unlock();
    return r;
}

uint16_t gap_central_get_conn_handle(void) {
    if(!gap_central.initialized) {
        return 0xFFFF;
    }
    gap_central_lock();
    uint16_t h = gap_central.conn_handle;
    gap_central_unlock();
    return h;
}

void gap_central_get_peer_addr(uint8_t out_addr[6], uint8_t* out_addr_type) {
    furi_check(out_addr);
    if(!gap_central.initialized) {
        memset(out_addr, 0, 6);
        if(out_addr_type) *out_addr_type = 0;
        return;
    }
    gap_central_lock();
    memcpy(out_addr, gap_central.peer_addr, 6);
    if(out_addr_type) *out_addr_type = gap_central.peer_addr_type;
    gap_central_unlock();
}

/* --- user-callback fan-out ---------------------------------------------- */

/* Snapshot the user event callback + context under lock, then invoke outside
 * it. Avoids holding state_mutex across user code.
 */
static void gap_central_fire_event(
    FuriHalBtCentralEvent ev,
    uint16_t char_handle,
    const uint8_t* data,
    size_t len) {
    FuriHalBtCentralEventCallback cb;
    void* ctx;

    gap_central_lock();
    cb = gap_central.event_cb;
    ctx = gap_central.event_ctx;
    gap_central_unlock();

    if(cb) {
        cb((FuriHalBtCentralConnection*)&gap_central, ev, char_handle, data, len, ctx);
    }
}

void gap_central_notify_user(
    FuriHalBtCentralEvent ev,
    uint16_t char_handle,
    const uint8_t* data,
    size_t len) {
    gap_central_fire_event(ev, char_handle, data, len);
}

/* --- adv report parsing ------------------------------------------------- */

/* Walk the raw LE meta advertising-report payload and fire the user scan
 * callback for each new peer. Payload layout (wire format):
 *   Num_Reports (1)
 *   for each report:
 *     Event_Type (1)
 *     Address_Type (1)
 *     Address (6)
 *     Length_Data (1)
 *     Data (Length_Data)
 *     RSSI (1)
 * See ble_events.c:hci_le_advertising_report_event_process.
 */
static void gap_central_handle_adv_report(const uint8_t* payload) {
    FuriHalBtCentralScanCallback cb;
    void* ctx;

    gap_central_lock();
    if(gap_central.state != GapCentralStateScanning) {
        gap_central_unlock();
        return;
    }
    cb = gap_central.scan_cb;
    ctx = gap_central.scan_ctx;
    gap_central_unlock();

    if(!cb) {
        return;
    }

    uint8_t num_reports = payload[0];
    const uint8_t* p = payload + 1;

    for(uint8_t i = 0; i < num_reports; i++) {
        uint8_t event_type = p[0];
        uint8_t addr_type = p[1];
        const uint8_t* addr = &p[2];
        uint8_t data_len = p[8];
        const uint8_t* data = &p[9];
        int8_t rssi = (int8_t)p[9 + data_len];

        (void)event_type;

        bool is_new;
        gap_central_lock();
        is_new = gap_central_scan_ring_mark(addr);
        gap_central_unlock();

        if(is_new) {
            FURI_LOG_D(
                TAG,
                "Adv %02x:%02x:%02x:%02x:%02x:%02x rssi=%d len=%u",
                addr[5],
                addr[4],
                addr[3],
                addr[2],
                addr[1],
                addr[0],
                rssi,
                data_len);

            FuriHalBtCentralScanResult result = {0};
            result.addr_type = addr_type;
            memcpy(result.addr, addr, 6);
            result.rssi = rssi;
            if(data_len > FURI_HAL_BT_CENTRAL_ADV_DATA_MAX) {
                data_len = FURI_HAL_BT_CENTRAL_ADV_DATA_MAX;
            }
            result.data_len = data_len;
            memcpy(result.data, data, data_len);
            cb(&result, ctx);
        }

        p += 1 + 1 + 6 + 1 + data_len + 1;
    }
}

/* --- connection complete ------------------------------------------------ */

static void gap_central_handle_conn_complete(
    uint8_t status,
    uint16_t conn_handle,
    uint8_t peer_addr_type,
    const uint8_t* peer_addr) {
    if(status == 0) {
        gap_central_lock();
        gap_central.state = GapCentralStateConnected;
        gap_central.conn_handle = conn_handle;
        gap_central.peer_addr_type = peer_addr_type;
        memcpy(gap_central.peer_addr, peer_addr, 6);
        gap_central_unlock();

        FURI_LOG_I(TAG, "Connected handle=0x%04x", conn_handle);
        gap_central_fire_event(FuriHalBtCentralEventConnected, 0, NULL, 0);
    } else {
        gap_central_lock();
        gap_central.state = GapCentralStateIdle;
        gap_central.conn_handle = 0xFFFF;
        gap_central_unlock();

        FURI_LOG_E(TAG, "Connect failed status=0x%02x", status);
        gap_central_fire_event(FuriHalBtCentralEventError, 0, NULL, 0);
    }
}

/* --- dispatcher entry --------------------------------------------------- */

BleEventAckStatus gap_central_handle_event(void* pckt) {
    furi_check(pckt);

    if(!gap_central.initialized) {
        return BleEventNotAck;
    }

    hci_uart_pckt* uart_pckt = (hci_uart_pckt*)pckt;
    if(uart_pckt->type != HCI_EVENT_PKT_TYPE) {
        return BleEventNotAck;
    }

    hci_event_pckt* event_pckt = (hci_event_pckt*)uart_pckt->data;

    switch(event_pckt->evt) {
    case HCI_LE_META_EVT_CODE: {
        evt_le_meta_event* meta = (evt_le_meta_event*)event_pckt->data;
        switch(meta->subevent) {
        case HCI_LE_ADVERTISING_REPORT_SUBEVT_CODE:
            gap_central_handle_adv_report(meta->data);
            return BleEventAckFlowEnable;

        case HCI_LE_CONNECTION_COMPLETE_SUBEVT_CODE: {
            hci_le_connection_complete_event_rp0* ev =
                (hci_le_connection_complete_event_rp0*)meta->data;
            /* Only claim if we initiated a connect. If peripheral got the
             * link instead, let its handler see it.
             */
            gap_central_lock();
            bool ours = (gap_central.state == GapCentralStateConnecting);
            gap_central_unlock();
            if(!ours) {
                return BleEventNotAck;
            }
            gap_central_handle_conn_complete(
                ev->Status, ev->Connection_Handle, ev->Peer_Address_Type, ev->Peer_Address);
            return BleEventAckFlowEnable;
        }

        case HCI_LE_ENHANCED_CONNECTION_COMPLETE_SUBEVT_CODE: {
            hci_le_enhanced_connection_complete_event_rp0* ev =
                (hci_le_enhanced_connection_complete_event_rp0*)meta->data;
            gap_central_lock();
            bool ours = (gap_central.state == GapCentralStateConnecting);
            gap_central_unlock();
            if(!ours) {
                return BleEventNotAck;
            }
            gap_central_handle_conn_complete(
                ev->Status, ev->Connection_Handle, ev->Peer_Address_Type, ev->Peer_Address);
            return BleEventAckFlowEnable;
        }

        default:
            return BleEventNotAck;
        }
    }

    case HCI_DISCONNECTION_COMPLETE_EVT_CODE: {
        hci_disconnection_complete_event_rp0* ev =
            (hci_disconnection_complete_event_rp0*)event_pckt->data;

        gap_central_lock();
        bool ours = (gap_central.state == GapCentralStateConnected ||
                     gap_central.state == GapCentralStateConnecting) &&
                    gap_central.conn_handle == ev->Connection_Handle;
        gap_central_unlock();

        if(!ours) {
            return BleEventNotAck;
        }

        FURI_LOG_I(
            TAG, "Disconnected handle=0x%04x reason=0x%02x", ev->Connection_Handle, ev->Reason);

        /* Clear GATT client state before announcing disconnect upward. */
        gatt_client_reset();

        gap_central_lock();
        gap_central.state = GapCentralStateIdle;
        gap_central.conn_handle = 0xFFFF;
        gap_central_unlock();

        gap_central_fire_event(FuriHalBtCentralEventDisconnected, 0, NULL, 0);
        return BleEventAckFlowEnable;
    }

    case HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE: {
        evt_blecore_aci* vs_evt = (evt_blecore_aci*)event_pckt->data;
        switch(vs_evt->ecode) {
        case ACI_GAP_PROC_COMPLETE_VSEVT_CODE: {
            aci_gap_proc_complete_event_rp0* ev =
                (aci_gap_proc_complete_event_rp0*)vs_evt->data;
            if(ev->Procedure_Code != GAP_GENERAL_DISCOVERY_PROC) {
                return BleEventNotAck;
            }

            gap_central_lock();
            if(gap_central.state == GapCentralStateScanning) {
                gap_central.state = GapCentralStateIdle;
                gap_central.scan_cb = NULL;
                gap_central.scan_ctx = NULL;
            }
            gap_central_unlock();

            FURI_LOG_I(TAG, "Scan complete status=0x%02x", ev->Status);
            return BleEventAckFlowEnable;
        }
        default:
            return BleEventNotAck;
        }
    }

    default:
        return BleEventNotAck;
    }
}
