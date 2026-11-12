/** @file gap_central.c
 * BLE central GAP: scan + connect/disconnect + event dispatch
 *
 * State machine (every state that waits for the controller has a deadline):
 *
 *   Idle          -> Scanning      start_scan accepted by the controller
 *   Scanning      -> ScanStopping  stop_scan, connect, deinit, or the scan watchdog
 *   Scanning      -> Idle          GAP procedure-complete (general discovery, 10.24 s)
 *   ScanStopping  -> Idle          GAP procedure-complete; deadline -> Faulted
 *   Idle          -> Connecting    create_connection accepted
 *   Connecting    -> Connected     successful central connection-complete for the pending peer
 *   Connecting    -> Idle          failed connection-complete (terminal Error)
 *   Connecting    -> Cancelling    disconnect() or the connect backstop deadline
 *   Cancelling    -> Idle          failed connection-complete or procedure-complete
 *   Cancelling    -> Disconnecting successful connection-complete: the handle is terminated
 *   Cancelling    -> Faulted       deadline
 *   Connected     -> Disconnecting disconnect()
 *   Connected     -> Idle          disconnection-complete (remote or supervision timeout)
 *   Disconnecting -> Idle          disconnection-complete; deadline -> Faulted
 *   Faulted       -> (none)        every central call fails until deinit and init
 *
 * Locking: gap_central.mutex protects everything in this file. It is taken by
 * application threads, the BLE event thread and the deadline worker thread, held
 * across single ACI commands (whose responses arrive through the IPCC interrupt,
 * not the event thread), and never held while calling the application or
 * gatt_client. Waits (connect stopping a scan, deinit) poll the state without holding it.
 *
 * Delivery order: connection events come from two threads (BLE event thread and
 * deadline worker). gap_central.deliver_mutex serialises their delivery: it is
 * held across one connection callback, and under it the event is checked again;
 * a non-terminal event whose connect call has had its terminal event claimed
 * (or whose link sequence is no longer current, or after the barrier closed) is
 * dropped. So no callback of a connect call runs after its terminal event.
 *
 * Lock order (outer first; never acquire leftwards while holding one to the right):
 *   event dispatcher mutex (BLE event thread only)
 *     -> gap_central.deliver_mutex
 *       -> gap_central.mutex | gatt_client.mutex (never nested with each other)
 * deliver_mutex is taken only by gap_central_deliver(), only on the BLE event
 * thread and the deadline worker, never with gap_central.mutex or
 * gatt_client.mutex held, and is held for at most one callback invocation plus a
 * short gap_central.mutex section. No API function takes it, so a callback may
 * call the HAL (connect, disconnect, discover, write) without deadlock, and the
 * BLE event thread waits on the deadline worker for at most one callback. It is
 * recursive only as a guard against a hang: a nested delivery on the same
 * thread is not possible (API calls never deliver events).
 *
 * Callback barrier: every application callback is registered as in flight under
 * the mutex before it is called and unregistered afterwards. Deinit closes the
 * gate, waits for in-flight callbacks to return and joins the deadline worker
 * thread; the facade then unregisters the dispatcher handler (which waits for
 * the event thread to leave the handler list).
 *
 * The deadline worker is a thread of its own (not a FuriTimer) because it issues
 * ACI commands, logs and calls the application, which does not fit the 1 KB
 * stack of the timer service thread. Its stack holds ACI command buffers,
 * logging and an application callback that may call the HAL; no measurement
 * exists, hence the generous size.
 */

#include "gap_central.h"
#include "furi_ble/gatt_client.h"

#include "app_common.h"
#include <ble/ble.h>

#include <furi_hal.h>
#include <furi_hal_bt.h>
#include <furi.h>

#include <stdint.h>
#include <string.h>

#define TAG "BleGapCentral"

#define GAP_CENTRAL_HANDLE_NONE       (0xFFFFU)
#define GAP_CENTRAL_DISCONNECT_REASON (0x13U) /* Remote user terminated */
#define GAP_CENTRAL_ROLE_CENTRAL      (0x00U) /* connection-complete Role field */
#define GAP_CENTRAL_HCI_UNKNOWN_CONN  (0x02U) /* status after LE_Create_Connection_Cancel */
#define GAP_CENTRAL_LATE_MAX          (2U)

#define GAP_CENTRAL_SCAN_DEDUP_SLOTS  (64U)
#define GAP_CENTRAL_INFLIGHT_SLOTS    (4U)
#define GAP_CENTRAL_WORKER_STACK      (3072U)

/* Controller duplicate filtering during general discovery: on. Without it every
 * repeated advertisement crosses the transport and the BLE event thread before
 * the software dedup, and the asynchronous event pool holds only five frames
 * (CFG_TLBLE_EVT_QUEUE_LENGTH, app_conf.h:184); pool exhaustion can stall HCI
 * commands. The software dedup by (address type, address, report type) stays:
 * it is what guarantees one report per key to the application, also if the
 * controller passes a scan response and its advertising report separately. */
#define GAP_CENTRAL_SCAN_FILTER_DUPLICATES (1U)
#define GAP_CENTRAL_WORKER_FLAG_STOP  (1UL << 0)

typedef enum {
    GapCentralStateIdle,
    GapCentralStateScanning,
    GapCentralStateScanStopping,
    GapCentralStateConnecting,
    GapCentralStateCancelling,
    GapCentralStateConnected,
    GapCentralStateDisconnecting,
    GapCentralStateFaulted,
} GapCentralState;

static const char* const gap_central_state_names[] = {
    "Idle",
    "Scanning",
    "ScanStopping",
    "Connecting",
    "Cancelling",
    "Connected",
    "Disconnecting",
    "Faulted",
};

typedef struct {
    uint8_t addr_type;
    uint8_t report_type;
    uint8_t addr[6];
} GapCentralSeenReport;

typedef struct {
    FuriMutex* mutex;
    FuriMutex* deliver_mutex; /* serialises connection-event delivery, see top */
    FuriThread* worker;
    FuriHalBtCentralConnection* conn_token;

    GapCentralState state;
    bool deadline_armed;
    uint32_t deadline; /* furi tick at which the current state expires */

    /* Scan */
    FuriHalBtCentralScanCallback scan_cb;
    void* scan_ctx;
    bool scan_active; /* accepted start_scan whose end event is not delivered yet */
    GapCentralSeenReport seen[GAP_CENTRAL_SCAN_DEDUP_SLOTS];
    uint8_t seen_count;
    uint8_t seen_next; /* oldest entry, evicted when the table is full */

    /* Connection */
    FuriHalBtCentralEventCallback event_cb;
    void* event_ctx;
    bool conn_active; /* accepted connect whose terminal event is not delivered yet */
    uint32_t link_seq; /* incremented per accepted connect */
    FuriHalBtCentralEvent end_event; /* terminal event when the attempt ends while cancelling */
    bool cancel_cmd_ok;
    uint8_t peer_addr[6];
    uint8_t peer_addr_type;
    uint16_t link_handle; /* the central's link (Connected, Disconnecting, Faulted) */
    uint16_t stray_handle; /* unexpected central link being terminated */
    uint8_t late_conn_fail; /* failed connection-completes still expected from cancels */
    uint8_t late_proc_complete; /* procedure-completes still expected from cancels */

    /* Callback barrier */
    bool gate_closed;
    uint32_t inflight;
    FuriThreadId inflight_thread[GAP_CENTRAL_INFLIGHT_SLOTS];

    bool initialized;
} GapCentral;

static GapCentral gap_central;

/* One application callback prepared under the mutex, delivered after it. */
typedef struct {
    bool valid;
    bool is_scan;
    int8_t slot;
    FuriHalBtCentralScanCallback scan_cb;
    FuriHalBtCentralEventCallback event_cb;
    void* ctx;
    FuriHalBtCentralEvent event;
    uint16_t char_handle;
    bool terminal; /* connection event that ends its connect call */
    uint32_t link_seq; /* connect call the connection event belongs to */
} GapCentralEmit;

/* --- small helpers ------------------------------------------------------- */

static inline void gap_central_lock(void) {
    furi_check(furi_mutex_acquire(gap_central.mutex, FuriWaitForever) == FuriStatusOk);
}

static inline void gap_central_unlock(void) {
    furi_check(furi_mutex_release(gap_central.mutex) == FuriStatusOk);
}

/* Caller holds the mutex. deadline_ms == 0 disarms the deadline. */
static void gap_central_set_state(GapCentralState state, uint32_t deadline_ms) {
    if(state != gap_central.state) {
        FURI_LOG_I(
            TAG,
            "%s -> %s",
            gap_central_state_names[gap_central.state],
            gap_central_state_names[state]);
    }
    gap_central.state = state;
    gap_central.deadline_armed = (deadline_ms != 0);
    gap_central.deadline = furi_get_tick() + furi_ms_to_ticks(deadline_ms);
}

static inline uint8_t gap_central_inc_capped(uint8_t v) {
    return (v < GAP_CENTRAL_LATE_MAX) ? (uint8_t)(v + 1) : v;
}

/* Caller holds the mutex. */
static bool gap_central_in_callback_locked(void) {
    FuriThreadId self = furi_thread_get_current_id();
    for(size_t i = 0; i < GAP_CENTRAL_INFLIGHT_SLOTS; i++) {
        if(gap_central.inflight_thread[i] == self) return true;
    }
    return false;
}

/* Caller holds the mutex. Registers the callback as in flight. */
static void gap_central_emit_enter(GapCentralEmit* e) {
    e->valid = true;
    e->slot = -1;
    gap_central.inflight++;
    for(size_t i = 0; i < GAP_CENTRAL_INFLIGHT_SLOTS; i++) {
        if(gap_central.inflight_thread[i] == NULL) {
            gap_central.inflight_thread[i] = furi_thread_get_current_id();
            e->slot = (int8_t)i;
            break;
        }
    }
}

/* Caller holds the mutex. Prepares a connection event. A terminal event ends the
 * connect call: exactly one is delivered, nothing for that call follows it. */
static void gap_central_prepare_conn(
    GapCentralEmit* e,
    FuriHalBtCentralEvent event,
    uint16_t char_handle,
    bool terminal) {
    e->valid = false;
    if(!gap_central.conn_active) return;
    if(terminal) gap_central.conn_active = false;
    if(gap_central.gate_closed || !gap_central.event_cb) return;
    e->is_scan = false;
    e->event_cb = gap_central.event_cb;
    e->ctx = gap_central.event_ctx;
    e->event = event;
    e->char_handle = char_handle;
    e->terminal = terminal;
    e->link_seq = gap_central.link_seq;
    gap_central_emit_enter(e);
}

/* Caller holds the mutex. Prepares a scan callback (report or scan end). */
static void gap_central_prepare_scan(GapCentralEmit* e, bool end) {
    e->valid = false;
    if(!gap_central.scan_active) return;
    if(end) gap_central.scan_active = false;
    if(gap_central.gate_closed || !gap_central.scan_cb) return;
    e->is_scan = true;
    e->scan_cb = gap_central.scan_cb;
    e->ctx = gap_central.scan_ctx;
    gap_central_emit_enter(e);
}

/* Called WITHOUT gap_central.mutex and gatt_client.mutex. Invokes the prepared
 * callback, then leaves flight. A connection event is delivered under
 * deliver_mutex; a non-terminal one is dropped when, by then, its connect call
 * has had its terminal event claimed, a newer connect call exists, or the
 * barrier is closed. A terminal event is claimed once under the mutex and is
 * always delivered. */
static void gap_central_deliver(
    GapCentralEmit* e,
    const FuriHalBtCentralScanResult* result,
    const uint8_t* data,
    size_t len) {
    if(!e->valid) return;

    if(e->is_scan) {
        e->scan_cb(result, e->ctx);
    } else {
        furi_check(
            furi_mutex_acquire(gap_central.deliver_mutex, FuriWaitForever) == FuriStatusOk);
        bool current = e->terminal;
        if(!current) {
            gap_central_lock();
            current = gap_central.conn_active && !gap_central.gate_closed &&
                      gap_central.link_seq == e->link_seq;
            gap_central_unlock();
        }
        if(current) {
            e->event_cb(gap_central.conn_token, e->event, e->char_handle, data, len, e->ctx);
        } else {
            FURI_LOG_D(TAG, "Stale event %u of link %lu dropped", e->event, e->link_seq);
        }
        furi_check(furi_mutex_release(gap_central.deliver_mutex) == FuriStatusOk);
    }

    gap_central_lock();
    furi_check(gap_central.inflight > 0);
    gap_central.inflight--;
    if(e->slot >= 0) gap_central.inflight_thread[e->slot] = NULL;
    gap_central_unlock();
    e->valid = false;
}

static void gap_central_deliver_scan_end(GapCentralEmit* e) {
    FuriHalBtCentralScanResult result;
    memset(&result, 0, sizeof(result));
    result.report_type = FURI_HAL_BT_CENTRAL_SCAN_REPORT_END;
    gap_central_deliver(e, &result, NULL, 0);
}

/* Returns true when (addr_type, addr, report_type) was not seen in this scan.
 * A full table evicts its oldest entry. Caller holds the mutex. */
static bool gap_central_scan_mark(uint8_t addr_type, const uint8_t addr[6], uint8_t report_type) {
    for(uint8_t i = 0; i < gap_central.seen_count; i++) {
        const GapCentralSeenReport* s = &gap_central.seen[i];
        if(s->addr_type == addr_type && s->report_type == report_type &&
           memcmp(s->addr, addr, 6) == 0) {
            return false;
        }
    }
    GapCentralSeenReport* slot;
    if(gap_central.seen_count < GAP_CENTRAL_SCAN_DEDUP_SLOTS) {
        slot = &gap_central.seen[gap_central.seen_count++];
    } else {
        slot = &gap_central.seen[gap_central.seen_next];
        gap_central.seen_next = (gap_central.seen_next + 1) % GAP_CENTRAL_SCAN_DEDUP_SLOTS;
    }
    slot->addr_type = addr_type;
    slot->report_type = report_type;
    memcpy(slot->addr, addr, 6);
    return true;
}

/* Polls the state until it is outside @p state_mask or @p timeout_ms passes.
 * Holds no lock while sleeping. Returns the last state seen. */
static GapCentralState gap_central_wait_while(uint32_t state_mask, uint32_t timeout_ms) {
    const uint32_t start = furi_get_tick();
    const uint32_t timeout = furi_ms_to_ticks(timeout_ms);
    GapCentralState state;
    while(true) {
        gap_central_lock();
        state = gap_central.state;
        gap_central_unlock();
        if(!(state_mask & (1UL << state))) break;
        if(furi_get_tick() - start >= timeout) break;
        furi_delay_ms(GAP_CENTRAL_WAIT_POLL_MS);
    }
    return state;
}

#define GAP_CENTRAL_MASK(s) (1UL << (GapCentralState##s))

/* --- deadlines (deadline worker thread) --------------------------------- */

static void gap_central_tick(void) {
    GapCentralEmit emit = {0};
    bool scan_end = false;

    gap_central_lock();
    if(!gap_central.initialized || gap_central.gate_closed || !gap_central.deadline_armed ||
       (int32_t)(furi_get_tick() - gap_central.deadline) < 0) {
        gap_central_unlock();
        return;
    }

    tBleStatus status;
    switch(gap_central.state) {
    case GapCentralStateScanning:
        /* Watchdog: the controller should have ended the procedure after 10.24 s. */
        status = aci_gap_terminate_gap_proc(GAP_GENERAL_DISCOVERY_PROC);
        FURI_LOG_W(TAG, "Scan watchdog, terminate status 0x%02X", status);
        gap_central_set_state(GapCentralStateScanStopping, GAP_CENTRAL_SCAN_STOP_TIMEOUT_MS);
        break;
    case GapCentralStateScanStopping:
        FURI_LOG_E(TAG, "No procedure-complete after scan stop");
        gap_central_set_state(GapCentralStateFaulted, 0);
        gap_central_prepare_scan(&emit, true);
        scan_end = true;
        break;
    case GapCentralStateConnecting:
        /* Backstop: the application normally cancels much earlier. */
        status = aci_gap_terminate_gap_proc(GAP_DIRECT_CONNECTION_ESTABLISHMENT_PROC);
        FURI_LOG_W(TAG, "Connect backstop, cancel status 0x%02X", status);
        gap_central.end_event = FuriHalBtCentralEventError;
        gap_central.cancel_cmd_ok = (status == BLE_STATUS_SUCCESS);
        gap_central_set_state(GapCentralStateCancelling, GAP_CENTRAL_CANCEL_TIMEOUT_MS);
        break;
    case GapCentralStateCancelling:
    case GapCentralStateDisconnecting:
        FURI_LOG_E(TAG, "No completion event, HAL faulted");
        gap_central_set_state(GapCentralStateFaulted, 0);
        gap_central_prepare_conn(
            &emit, FuriHalBtCentralEventError, FURI_HAL_BT_CENTRAL_ERROR_LINK, true);
        break;
    default:
        gap_central.deadline_armed = false;
        break;
    }
    gap_central_unlock();

    if(scan_end) {
        gap_central_deliver_scan_end(&emit);
    } else {
        gap_central_deliver(&emit, NULL, NULL, 0);
    }
}

static int32_t gap_central_worker(void* context) {
    UNUSED(context);
    while(true) {
        uint32_t flags = furi_thread_flags_wait(
            GAP_CENTRAL_WORKER_FLAG_STOP, FuriFlagWaitAny, furi_ms_to_ticks(GAP_CENTRAL_TICK_MS));
        if(!(flags & FuriFlagError) && (flags & GAP_CENTRAL_WORKER_FLAG_STOP)) {
            break;
        }
        gap_central_tick();
        gatt_client_tick();
    }
    return 0;
}

/* --- init / deinit ------------------------------------------------------- */

void gap_central_init(FuriHalBtCentralConnection* conn) {
    furi_check(!gap_central.initialized);
    furi_check(conn);

    memset(&gap_central, 0, sizeof(gap_central));
    gap_central.mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    gap_central.deliver_mutex = furi_mutex_alloc(FuriMutexTypeRecursive);
    gap_central.conn_token = conn;
    gap_central.state = GapCentralStateIdle;
    gap_central.link_handle = GAP_CENTRAL_HANDLE_NONE;
    gap_central.stray_handle = GAP_CENTRAL_HANDLE_NONE;
    gap_central.initialized = true;

    gap_central.worker = furi_thread_alloc_ex(
        "BleCentralDeadline", GAP_CENTRAL_WORKER_STACK, gap_central_worker, NULL);
    furi_thread_start(gap_central.worker);

    FURI_LOG_I(TAG, "Init");
}

void gap_central_deinit(void) {
    furi_check(gap_central.initialized);

    gap_central_lock();
    if(gap_central_in_callback_locked()) {
        gap_central_unlock();
        furi_crash("Central deinit from a HAL callback");
    }

    /* 1. Scan */
    if(gap_central.state == GapCentralStateScanning) {
        tBleStatus status = aci_gap_terminate_gap_proc(GAP_GENERAL_DISCOVERY_PROC);
        if(status) FURI_LOG_W(TAG, "deinit: scan stop status 0x%02X", status);
        gap_central_set_state(GapCentralStateScanStopping, GAP_CENTRAL_SCAN_STOP_TIMEOUT_MS);
    }
    gap_central_unlock();
    gap_central_wait_while(
        GAP_CENTRAL_MASK(Scanning) | GAP_CENTRAL_MASK(ScanStopping),
        GAP_CENTRAL_DEINIT_STEP_TIMEOUT_MS);

    /* 2. Connection attempt */
    gap_central_disconnect();
    gap_central_wait_while(
        GAP_CENTRAL_MASK(Connecting) | GAP_CENTRAL_MASK(Cancelling),
        GAP_CENTRAL_DEINIT_STEP_TIMEOUT_MS);

    /* 3. Open link */
    gap_central_disconnect();
    GapCentralState state = gap_central_wait_while(
        GAP_CENTRAL_MASK(Connected) | GAP_CENTRAL_MASK(Disconnecting),
        GAP_CENTRAL_DEINIT_STEP_TIMEOUT_MS);
    if(state != GapCentralStateIdle) {
        FURI_LOG_W(TAG, "deinit: unresolved state %s", gap_central_state_names[state]);
    }

    /* 4. Barrier: no callback starts after this, wait for running ones. Bounded
     * only by the application's callbacks, which must not block. */
    gap_central_lock();
    gap_central.gate_closed = true;
    gap_central_unlock();
    while(true) {
        gap_central_lock();
        uint32_t inflight = gap_central.inflight;
        gap_central_unlock();
        if(inflight == 0) break;
        furi_delay_ms(1);
    }

    /* 5. Deadline worker: after the join no tick runs. */
    furi_thread_flags_set(furi_thread_get_id(gap_central.worker), GAP_CENTRAL_WORKER_FLAG_STOP);
    furi_thread_join(gap_central.worker);
    furi_thread_free(gap_central.worker);
    gap_central.worker = NULL;

    FURI_LOG_I(TAG, "Deinit barrier passed");
}

void gap_central_free(void) {
    furi_check(gap_central.initialized);
    furi_check(gap_central.worker == NULL);
    furi_mutex_free(gap_central.deliver_mutex);
    furi_mutex_free(gap_central.mutex);
    memset(&gap_central, 0, sizeof(gap_central));
    FURI_LOG_I(TAG, "Free");
}

/* --- scan ---------------------------------------------------------------- */

bool gap_central_start_scan(FuriHalBtCentralScanCallback cb, void* ctx) {
    furi_check(cb);
    furi_check(gap_central.initialized);

    bool ok = false;
    gap_central_lock();
    do {
        if(gap_central.state != GapCentralStateIdle) {
            FURI_LOG_E(
                TAG, "start_scan: bad state %s", gap_central_state_names[gap_central.state]);
            break;
        }
        tBleStatus status = aci_gap_start_general_discovery_proc(
            GAP_CENTRAL_SCAN_INTERVAL,
            GAP_CENTRAL_SCAN_WINDOW,
            GAP_PUBLIC_ADDR,
            GAP_CENTRAL_SCAN_FILTER_DUPLICATES);
        if(status) {
            FURI_LOG_E(TAG, "start_general_discovery_proc failed 0x%02X", status);
            break;
        }
        gap_central.scan_cb = cb;
        gap_central.scan_ctx = ctx;
        gap_central.scan_active = true;
        gap_central.seen_count = 0;
        gap_central.seen_next = 0;
        gap_central_set_state(GapCentralStateScanning, GAP_CENTRAL_SCAN_WATCHDOG_MS);
        ok = true;
    } while(false);
    gap_central_unlock();
    return ok;
}

bool gap_central_stop_scan(void) {
    furi_check(gap_central.initialized);

    gap_central_lock();
    if(gap_central.state == GapCentralStateScanning) {
        tBleStatus status = aci_gap_terminate_gap_proc(GAP_GENERAL_DISCOVERY_PROC);
        /* A rejection usually means the procedure just ended and its
         * procedure-complete is queued; the deadline covers the rest. */
        if(status) FURI_LOG_W(TAG, "terminate_gap_proc(scan) status 0x%02X", status);
        gap_central_set_state(GapCentralStateScanStopping, GAP_CENTRAL_SCAN_STOP_TIMEOUT_MS);
    }
    bool ok = (gap_central.state != GapCentralStateFaulted);
    gap_central_unlock();
    return ok;
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

    /* A running scan is stopped first, waiting on this thread only. */
    gap_central_lock();
    bool scan_running = (gap_central.state == GapCentralStateScanning) ||
                        (gap_central.state == GapCentralStateScanStopping);
    if(scan_running && gap_central_in_callback_locked()) {
        gap_central_unlock();
        FURI_LOG_E(TAG, "connect: cannot stop a scan from a HAL callback");
        return false;
    }
    if(gap_central.state == GapCentralStateScanning) {
        tBleStatus status = aci_gap_terminate_gap_proc(GAP_GENERAL_DISCOVERY_PROC);
        if(status) FURI_LOG_W(TAG, "connect: scan stop status 0x%02X", status);
        gap_central_set_state(GapCentralStateScanStopping, GAP_CENTRAL_SCAN_STOP_TIMEOUT_MS);
    }
    gap_central_unlock();

    if(scan_running) {
        GapCentralState state = gap_central_wait_while(
            GAP_CENTRAL_MASK(ScanStopping), GAP_CENTRAL_CONNECT_SCAN_STOP_WAIT_MS);
        if(state == GapCentralStateScanStopping) {
            FURI_LOG_E(TAG, "connect: scan did not stop in time");
            return false;
        }
    }

    bool ok = false;
    gap_central_lock();
    do {
        if(gap_central.state != GapCentralStateIdle) {
            /* The live link's callback and context stay untouched. */
            FURI_LOG_E(TAG, "connect: bad state %s", gap_central_state_names[gap_central.state]);
            break;
        }
        tBleStatus status = aci_gap_create_connection(
            GAP_CENTRAL_SCAN_INTERVAL,
            GAP_CENTRAL_SCAN_WINDOW,
            addr_type,
            addr,
            GAP_PUBLIC_ADDR,
            GAP_CENTRAL_CONN_INTERVAL_MIN,
            GAP_CENTRAL_CONN_INTERVAL_MAX,
            GAP_CENTRAL_CONN_LATENCY,
            GAP_CENTRAL_SUPERVISION_TIMEOUT,
            GAP_CENTRAL_CE_LENGTH_MIN,
            GAP_CENTRAL_CE_LENGTH_MAX);
        if(status) {
            FURI_LOG_E(TAG, "create_connection failed 0x%02X", status);
            break;
        }
        memcpy(gap_central.peer_addr, addr, 6);
        gap_central.peer_addr_type = addr_type;
        gap_central.event_cb = cb;
        gap_central.event_ctx = ctx;
        gap_central.conn_active = true;
        gap_central.link_seq++;
        gap_central.end_event = FuriHalBtCentralEventDisconnected;
        gap_central.cancel_cmd_ok = false;
        gap_central_set_state(GapCentralStateConnecting, GAP_CENTRAL_CONNECT_TIMEOUT_MS);
        ok = true;
    } while(false);
    gap_central_unlock();
    return ok;
}

void gap_central_disconnect(void) {
    furi_check(gap_central.initialized);

    gap_central_lock();
    tBleStatus status;
    switch(gap_central.state) {
    case GapCentralStateConnecting:
        /* Cancel the direct connection establishment procedure (0x40). */
        status = aci_gap_terminate_gap_proc(GAP_DIRECT_CONNECTION_ESTABLISHMENT_PROC);
        if(status) FURI_LOG_W(TAG, "cancel status 0x%02X", status);
        gap_central.end_event = FuriHalBtCentralEventDisconnected;
        gap_central.cancel_cmd_ok = (status == BLE_STATUS_SUCCESS);
        gap_central_set_state(GapCentralStateCancelling, GAP_CENTRAL_CANCEL_TIMEOUT_MS);
        break;
    case GapCentralStateConnected:
        status = aci_gap_terminate(gap_central.link_handle, GAP_CENTRAL_DISCONNECT_REASON);
        if(status) FURI_LOG_W(TAG, "terminate status 0x%02X", status);
        gap_central.end_event = FuriHalBtCentralEventDisconnected;
        gap_central_set_state(GapCentralStateDisconnecting, GAP_CENTRAL_DISCONNECT_TIMEOUT_MS);
        break;
    default:
        /* Idle, scanning, already cancelling or disconnecting, faulted: nothing to do. */
        break;
    }
    gap_central_unlock();
}

bool gap_central_is_connected(void) {
    if(!gap_central.initialized) return false;
    gap_central_lock();
    bool r = (gap_central.state == GapCentralStateConnected);
    gap_central_unlock();
    return r;
}

bool gap_central_is_faulted(void) {
    if(!gap_central.initialized) return false;
    gap_central_lock();
    bool r = (gap_central.state == GapCentralStateFaulted);
    gap_central_unlock();
    return r;
}

void gap_central_notify_user(
    uint32_t link_seq,
    FuriHalBtCentralEvent ev,
    uint16_t char_handle,
    const uint8_t* data,
    size_t len) {
    GapCentralEmit emit = {0};
    gap_central_lock();
    if(gap_central.initialized && link_seq == gap_central.link_seq &&
       gap_central.state == GapCentralStateConnected) {
        gap_central_prepare_conn(&emit, ev, char_handle, false);
    }
    gap_central_unlock();
    gap_central_deliver(&emit, NULL, data, len);
}

/* --- adv report parsing ------------------------------------------------- */

/* Raw LE meta advertising-report payload:
 *   Num_Reports (1), then per report: Event_Type (1), Address_Type (1),
 *   Address (6), Length_Data (1), Data (Length_Data), RSSI (1).
 * Event_Type 0x04 is a scan response (active scanning).
 */
static void gap_central_handle_adv_report(const uint8_t* payload, uint8_t payload_len) {
    if(payload_len < 1) return;
    uint8_t num_reports = payload[0];
    const uint8_t* p = payload + 1;
    const uint8_t* end = payload + payload_len;

    for(uint8_t i = 0; i < num_reports; i++) {
        if(end - p < 10) return;
        uint8_t event_type = p[0];
        uint8_t addr_type = p[1];
        const uint8_t* addr = &p[2];
        uint8_t data_len = p[8];
        if(data_len > FURI_HAL_BT_CENTRAL_ADV_DATA_MAX || end - p < 10 + data_len) return;
        const uint8_t* data = &p[9];
        int8_t rssi = (int8_t)p[9 + data_len];

        GapCentralEmit emit = {0};
        gap_central_lock();
        bool scanning = (gap_central.state == GapCentralStateScanning ||
                         gap_central.state == GapCentralStateScanStopping);
        if(scanning && gap_central_scan_mark(addr_type, addr, event_type)) {
            gap_central_prepare_scan(&emit, false);
        }
        gap_central_unlock();

        if(emit.valid) {
            FuriHalBtCentralScanResult result;
            memset(&result, 0, sizeof(result));
            result.addr_type = addr_type;
            memcpy(result.addr, addr, 6);
            result.rssi = rssi;
            result.data_len = data_len;
            memcpy(result.data, data, data_len);
            result.report_type = event_type;
            gap_central_deliver(&emit, &result, NULL, 0);
        }

        p += 10 + data_len;
    }
}

/* --- connection complete (legacy and enhanced) -------------------------- */

/* Identity policy: the peer matches when its address type (bit 0 of the
 * reported type, so identity types 0x02/0x03 compare as public/random) and its
 * address equal the pending peer, or, for the enhanced event, when the peer's
 * resolvable private address equals the pending address (connect by RPA).
 * Privacy is not enabled on this device, so the plain comparison is the
 * expected path. */
static bool gap_central_peer_matches(
    uint8_t peer_addr_type,
    const uint8_t* peer_addr,
    const uint8_t* peer_rpa) {
    if((peer_addr_type & 0x01U) == (gap_central.peer_addr_type & 0x01U) &&
       memcmp(peer_addr, gap_central.peer_addr, 6) == 0) {
        return true;
    }
    if(peer_rpa && peer_addr_type >= 0x02U && memcmp(peer_rpa, gap_central.peer_addr, 6) == 0) {
        return true;
    }
    return false;
}

/* Caller holds the mutex. Terminates a link from the event thread. */
static void gap_central_terminate_locked(uint16_t handle) {
    tBleStatus status = aci_gap_terminate(handle, GAP_CENTRAL_DISCONNECT_REASON);
    if(status) FURI_LOG_W(TAG, "terminate 0x%04X status 0x%02X", handle, status);
}

static BleEventAckStatus gap_central_handle_conn_complete(
    uint8_t status,
    uint16_t handle,
    uint8_t role,
    uint8_t peer_addr_type,
    const uint8_t* peer_addr,
    const uint8_t* peer_rpa) {
    GapCentralEmit emit = {0};
    bool attach = false;
    uint32_t seq = 0;
    BleEventAckStatus ack = BleEventAckFlowEnable;

    gap_central_lock();
    const GapCentralState state = gap_central.state;

    if(status != 0) {
        /* Role and peer fields of a failed event are not relied upon. */
        if(status == GAP_CENTRAL_HCI_UNKNOWN_CONN && gap_central.late_conn_fail > 0) {
            gap_central.late_conn_fail--;
            FURI_LOG_I(TAG, "Late cancel completion consumed");
        } else if(state == GapCentralStateConnecting) {
            FURI_LOG_E(TAG, "Connect failed status=0x%02X", status);
            gap_central_set_state(GapCentralStateIdle, 0);
            gap_central_prepare_conn(
                &emit, FuriHalBtCentralEventError, FURI_HAL_BT_CENTRAL_ERROR_LINK, true);
        } else if(state == GapCentralStateCancelling) {
            FURI_LOG_I(TAG, "Connect cancelled status=0x%02X", status);
            if(gap_central.cancel_cmd_ok) {
                gap_central.late_proc_complete =
                    gap_central_inc_capped(gap_central.late_proc_complete);
            }
            gap_central_set_state(GapCentralStateIdle, 0);
            gap_central_prepare_conn(
                &emit, gap_central.end_event, FURI_HAL_BT_CENTRAL_ERROR_LINK, true);
        } else {
            /* No central procedure outstanding: not ours. */
            ack = BleEventNotAck;
        }
    } else if(role != GAP_CENTRAL_ROLE_CENTRAL) {
        /* Peripheral link: owned by gap.c. */
        ack = BleEventNotAck;
    } else if(
        state == GapCentralStateConnecting &&
        gap_central_peer_matches(peer_addr_type, peer_addr, peer_rpa)) {
        FURI_LOG_I(TAG, "Connected handle=0x%04X", handle);
        gap_central.link_handle = handle;
        gap_central_set_state(GapCentralStateConnected, 0);
        gap_central_prepare_conn(&emit, FuriHalBtCentralEventConnected, 0, false);
        attach = true;
        seq = gap_central.link_seq;
    } else if(state == GapCentralStateConnecting || state == GapCentralStateCancelling) {
        /* Cancel raced with success (or the peer did not match): drop the link at
         * once, report the attempt's terminal event on its disconnection. */
        FURI_LOG_W(TAG, "Link 0x%04X while %s, terminating", handle, gap_central_state_names[state]);
        if(state == GapCentralStateConnecting) {
            gap_central.end_event = FuriHalBtCentralEventError;
        } else if(gap_central.cancel_cmd_ok) {
            gap_central.late_proc_complete = gap_central_inc_capped(gap_central.late_proc_complete);
        }
        gap_central.link_handle = handle;
        gap_central_terminate_locked(handle);
        gap_central_set_state(GapCentralStateDisconnecting, GAP_CENTRAL_DISCONNECT_TIMEOUT_MS);
    } else {
        /* Central link nobody asked for (late completion after the attempt was
         * resolved, or while faulted): terminate it and consume its events. */
        FURI_LOG_W(TAG, "Stray central link 0x%04X, terminating", handle);
        gap_central.stray_handle = handle;
        gap_central_terminate_locked(handle);
    }
    gap_central_unlock();

    if(attach) gatt_client_attach(handle, seq);
    gap_central_deliver(&emit, NULL, NULL, 0);
    return ack;
}

static BleEventAckStatus gap_central_handle_disconnect(uint8_t status, uint16_t handle) {
    GapCentralEmit emit = {0};
    bool reset_gatt = false;

    gap_central_lock();
    if(handle == GAP_CENTRAL_HANDLE_NONE ||
       (handle != gap_central.link_handle && handle != gap_central.stray_handle)) {
        gap_central_unlock();
        return BleEventNotAck;
    }

    if(status != 0) {
        /* The link is still up; the Disconnecting deadline decides. */
        FURI_LOG_W(TAG, "Disconnection 0x%04X failed status=0x%02X", handle, status);
    } else if(handle == gap_central.stray_handle) {
        gap_central.stray_handle = GAP_CENTRAL_HANDLE_NONE;
        FURI_LOG_I(TAG, "Stray link 0x%04X gone", handle);
    } else {
        gap_central.link_handle = GAP_CENTRAL_HANDLE_NONE;
        reset_gatt = true;
        if(gap_central.state == GapCentralStateConnected) {
            FURI_LOG_I(TAG, "Link lost handle=0x%04X", handle);
            gap_central_set_state(GapCentralStateIdle, 0);
            gap_central_prepare_conn(&emit, FuriHalBtCentralEventDisconnected, 0, true);
        } else if(gap_central.state == GapCentralStateDisconnecting) {
            gap_central_set_state(GapCentralStateIdle, 0);
            gap_central_prepare_conn(
                &emit, gap_central.end_event, FURI_HAL_BT_CENTRAL_ERROR_LINK, true);
        } else {
            /* Faulted: terminal event was delivered already. */
            FURI_LOG_I(TAG, "Link 0x%04X gone while %s", handle, gap_central_state_names[gap_central.state]);
        }
    }
    gap_central_unlock();

    /* GATT bookkeeping is cleared before the application hears about it. */
    if(reset_gatt) gatt_client_reset();
    gap_central_deliver(&emit, NULL, NULL, 0);
    return BleEventAckFlowEnable;
}

static BleEventAckStatus gap_central_handle_proc_complete(uint8_t proc, uint8_t status) {
    GapCentralEmit emit = {0};
    bool scan_end = false;

    gap_central_lock();
    if(proc == GAP_GENERAL_DISCOVERY_PROC) {
        if(gap_central.state == GapCentralStateScanning ||
           gap_central.state == GapCentralStateScanStopping) {
            FURI_LOG_I(TAG, "Scan complete status=0x%02X", status);
            gap_central_set_state(GapCentralStateIdle, 0);
            gap_central_prepare_scan(&emit, true);
            scan_end = true;
        }
    } else if(proc == GAP_DIRECT_CONNECTION_ESTABLISHMENT_PROC) {
        if(gap_central.late_proc_complete > 0) {
            gap_central.late_proc_complete--;
            FURI_LOG_I(TAG, "Late cancel procedure-complete consumed");
        } else if(gap_central.state == GapCentralStateCancelling) {
            FURI_LOG_I(TAG, "Connect cancelled (procedure-complete 0x%02X)", status);
            /* The controller may still report the cancel as a failed connection-complete. */
            gap_central.late_conn_fail = gap_central_inc_capped(gap_central.late_conn_fail);
            gap_central_set_state(GapCentralStateIdle, 0);
            gap_central_prepare_conn(
                &emit, gap_central.end_event, FURI_HAL_BT_CENTRAL_ERROR_LINK, true);
        } else if(gap_central.state == GapCentralStateConnecting && status != 0) {
            FURI_LOG_E(TAG, "Connect procedure failed status=0x%02X", status);
            gap_central.late_conn_fail = gap_central_inc_capped(gap_central.late_conn_fail);
            gap_central_set_state(GapCentralStateIdle, 0);
            gap_central_prepare_conn(
                &emit, FuriHalBtCentralEventError, FURI_HAL_BT_CENTRAL_ERROR_LINK, true);
        }
    }
    gap_central_unlock();

    if(scan_end) {
        gap_central_deliver_scan_end(&emit);
    } else {
        gap_central_deliver(&emit, NULL, NULL, 0);
    }
    return BleEventAckFlowEnable;
}

/* True when @p handle belongs to the central (its link or a stray link). */
static bool gap_central_owns_handle(uint16_t handle) {
    gap_central_lock();
    bool r = handle != GAP_CENTRAL_HANDLE_NONE &&
             (handle == gap_central.link_handle || handle == gap_central.stray_handle);
    gap_central_unlock();
    return r;
}

/* The peer asks for other connection parameters (L2CAP connection parameter
 * update request). The stack requires an answer; the central rejects the
 * request, so the link keeps the parameters create_connection set, which the
 * deadlines here and in gatt_client are built around. A rejection carries only
 * the result on air; the central's own (valid) parameters fill the other fields
 * of the command. Like gap_central_terminate_locked(), the command is sent with
 * the mutex held. Requests on links the central does not own are left to the
 * peripheral handlers. */
static BleEventAckStatus
    gap_central_handle_l2cap_update_req(const aci_l2cap_connection_update_req_event_rp0* ev) {
    const uint16_t handle = ev->Connection_Handle;
    const uint8_t identifier = ev->Identifier;

    gap_central_lock();
    const bool owned = handle != GAP_CENTRAL_HANDLE_NONE &&
                       (handle == gap_central.link_handle || handle == gap_central.stray_handle);
    if(owned) {
        tBleStatus status = aci_l2cap_connection_parameter_update_resp(
            handle,
            GAP_CENTRAL_CONN_INTERVAL_MIN,
            GAP_CENTRAL_CONN_INTERVAL_MAX,
            GAP_CENTRAL_CONN_LATENCY,
            GAP_CENTRAL_SUPERVISION_TIMEOUT,
            GAP_CENTRAL_CE_LENGTH_MIN,
            GAP_CENTRAL_CE_LENGTH_MAX,
            identifier,
            0x00U /* Accept: 0x00 = reject */);
        FURI_LOG_D(
            TAG, "Parameter update request on 0x%04X rejected, status 0x%02X", handle, status);
    }
    gap_central_unlock();
    return owned ? BleEventAckFlowEnable : BleEventNotAck;
}

static inline uint16_t gap_central_read_u16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/* Vendor events whose parameters start with a 16-bit Connection_Handle. */
static bool gap_central_vs_has_handle(uint16_t ecode) {
    switch(ecode) {
    case ACI_GAP_PAIRING_COMPLETE_VSEVT_CODE:
    case ACI_GAP_PASS_KEY_REQ_VSEVT_CODE:
    case ACI_GAP_AUTHORIZATION_REQ_VSEVT_CODE:
    case ACI_GAP_ADDR_NOT_RESOLVED_VSEVT_CODE:
    case ACI_GAP_NUMERIC_COMPARISON_VALUE_VSEVT_CODE:
    case ACI_GAP_KEYPRESS_NOTIFICATION_VSEVT_CODE:
    case ACI_L2CAP_CONNECTION_UPDATE_RESP_VSEVT_CODE:
    case ACI_L2CAP_PROC_TIMEOUT_VSEVT_CODE:
    case ACI_L2CAP_CONNECTION_UPDATE_REQ_VSEVT_CODE:
    case ACI_L2CAP_COMMAND_REJECT_VSEVT_CODE:
        return true;
    case ACI_GATT_EATT_BEARER_VSEVT_CODE:
    case ACI_GATT_NOTIFICATION_COMPLETE_VSEVT_CODE:
        return false;
    default:
        /* GATT/ATT events 0x0C01..0x0C1F */
        return ecode >= ACI_GATT_ATTRIBUTE_MODIFIED_VSEVT_CODE &&
               ecode <= ACI_GATT_NOTIFICATION_EXT_VSEVT_CODE;
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
            if(event_pckt->plen < 1) return BleEventNotAck;
            gap_central_handle_adv_report(meta->data, event_pckt->plen - 1);
            return BleEventAckFlowEnable;

        case HCI_LE_CONNECTION_COMPLETE_SUBEVT_CODE: {
            hci_le_connection_complete_event_rp0* ev =
                (hci_le_connection_complete_event_rp0*)meta->data;
            return gap_central_handle_conn_complete(
                ev->Status,
                ev->Connection_Handle,
                ev->Role,
                ev->Peer_Address_Type,
                ev->Peer_Address,
                NULL);
        }

        case HCI_LE_ENHANCED_CONNECTION_COMPLETE_SUBEVT_CODE: {
            hci_le_enhanced_connection_complete_event_rp0* ev =
                (hci_le_enhanced_connection_complete_event_rp0*)meta->data;
            return gap_central_handle_conn_complete(
                ev->Status,
                ev->Connection_Handle,
                ev->Role,
                ev->Peer_Address_Type,
                ev->Peer_Address,
                ev->Peer_Resolvable_Private_Address);
        }

        /* Status (1) then Connection_Handle (2) */
        case HCI_LE_CONNECTION_UPDATE_COMPLETE_SUBEVT_CODE:
        case HCI_LE_READ_REMOTE_FEATURES_COMPLETE_SUBEVT_CODE:
        case HCI_LE_PHY_UPDATE_COMPLETE_SUBEVT_CODE:
            return gap_central_owns_handle(gap_central_read_u16(&meta->data[1])) ?
                       BleEventAckFlowEnable :
                       BleEventNotAck;

        /* Connection_Handle (2) first */
        case HCI_LE_DATA_LENGTH_CHANGE_SUBEVT_CODE:
        case HCI_LE_CHANNEL_SELECTION_ALGORITHM_SUBEVT_CODE:
            return gap_central_owns_handle(gap_central_read_u16(&meta->data[0])) ?
                       BleEventAckFlowEnable :
                       BleEventNotAck;

        default:
            return BleEventNotAck;
        }
    }

    case HCI_DISCONNECTION_COMPLETE_EVT_CODE: {
        hci_disconnection_complete_event_rp0* ev =
            (hci_disconnection_complete_event_rp0*)event_pckt->data;
        return gap_central_handle_disconnect(ev->Status, ev->Connection_Handle);
    }

    /* Status (1) then Connection_Handle (2) */
    case HCI_ENCRYPTION_CHANGE_EVT_CODE:
    case HCI_READ_REMOTE_VERSION_INFORMATION_COMPLETE_EVT_CODE:
    case HCI_ENCRYPTION_KEY_REFRESH_COMPLETE_EVT_CODE:
        return gap_central_owns_handle(gap_central_read_u16(&event_pckt->data[1])) ?
                   BleEventAckFlowEnable :
                   BleEventNotAck;

    case HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE: {
        evt_blecore_aci* vs_evt = (evt_blecore_aci*)event_pckt->data;
        if(vs_evt->ecode == ACI_GAP_PROC_COMPLETE_VSEVT_CODE) {
            aci_gap_proc_complete_event_rp0* ev = (aci_gap_proc_complete_event_rp0*)vs_evt->data;
            if(ev->Procedure_Code != GAP_GENERAL_DISCOVERY_PROC &&
               ev->Procedure_Code != GAP_DIRECT_CONNECTION_ESTABLISHMENT_PROC) {
                return BleEventNotAck;
            }
            return gap_central_handle_proc_complete(ev->Procedure_Code, ev->Status);
        }
        if(vs_evt->ecode == ACI_L2CAP_CONNECTION_UPDATE_REQ_VSEVT_CODE) {
            return gap_central_handle_l2cap_update_req(
                (const aci_l2cap_connection_update_req_event_rp0*)vs_evt->data);
        }
        if(gap_central_vs_has_handle(vs_evt->ecode) &&
           gap_central_owns_handle(gap_central_read_u16(vs_evt->data))) {
            /* Leftover events of the central link (the GATT client saw them
             * first) must not reach the peripheral handlers. */
            return BleEventAckFlowEnable;
        }
        return BleEventNotAck;
    }

    default:
        return BleEventNotAck;
    }
}
