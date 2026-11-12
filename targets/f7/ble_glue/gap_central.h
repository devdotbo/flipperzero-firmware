/** @file gap_central.h
 * BLE central GAP: scan + connect/disconnect + event dispatch (internal)
 *
 * Threads: API calls come from application threads; gap_central_handle_event()
 * runs on the BLE event thread; deadlines run on a deadline worker thread.
 * Application callbacks are invoked from the latter two, never from the
 * thread that calls the API, and never while gap_central.mutex or
 * gatt_client.mutex is held. Exception: connection callbacks run while
 * gap_central.deliver_mutex (the delivery lock) is held; no API function takes
 * that lock, so a callback may call the HAL (see the locking notes in
 * gap_central.c).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <furi_hal_bt_central.h>

#include "furi_ble/event_dispatcher.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Timing of the central HAL, all in one place ------------------------- */

/* Scanning, both for general discovery and for connection establishment.
 * Unit 0.625 ms. Interval == window: the radio scans 100 percent of the time.
 * General discovery is active scanning (scan responses are reported) and the
 * controller ends it by itself after 10.24 s with a procedure-complete. */
#define GAP_CENTRAL_SCAN_INTERVAL (0x0320U) /* 500 ms */
#define GAP_CENTRAL_SCAN_WINDOW   (0x0320U) /* 500 ms */

/* Connection parameters requested by create_connection. */
#define GAP_CENTRAL_CONN_INTERVAL_MIN   (12U) /* unit 1.25 ms: 15 ms */
#define GAP_CENTRAL_CONN_INTERVAL_MAX   (24U) /* unit 1.25 ms: 30 ms */
#define GAP_CENTRAL_CONN_LATENCY        (0U) /* connection events */
/* Supervision timeout, unit 10 ms: 200 * 10 ms = 2000 ms = 2 s. A disconnect the
 * peer does not acknowledge resolves at this timeout, so it bounds the stall
 * before the next connection. Spec minimum: (1 + latency) * interval_max * 2 =
 * 1 * 30 ms * 2 = 60 ms. GAP_CENTRAL_DISCONNECT_TIMEOUT_MS must stay above it. */
#define GAP_CENTRAL_SUPERVISION_TIMEOUT (200U) /* unit 10 ms: 2 s */
#define GAP_CENTRAL_CE_LENGTH_MIN       (16U) /* unit 0.625 ms: 10 ms */
#define GAP_CENTRAL_CE_LENGTH_MAX       (16U) /* unit 0.625 ms: 10 ms */

/* Deadlines, milliseconds. Checked by the deadline worker every GAP_CENTRAL_TICK_MS. */
#define GAP_CENTRAL_TICK_MS                   (50U)
#define GAP_CENTRAL_SCAN_WATCHDOG_MS          (12000U) /* controller ends the scan after 10240 */
#define GAP_CENTRAL_SCAN_STOP_TIMEOUT_MS      (1000U) /* ScanStopping -> Faulted */
#define GAP_CENTRAL_CONNECT_TIMEOUT_MS        (10000U) /* Connecting -> Cancelling (backstop) */
#define GAP_CENTRAL_CANCEL_TIMEOUT_MS         (1000U) /* Cancelling -> Faulted */
#define GAP_CENTRAL_DISCONNECT_TIMEOUT_MS     (6000U) /* Disconnecting -> Faulted, > supervision */
#define GAP_CENTRAL_CONNECT_SCAN_STOP_WAIT_MS (500U) /* connect stops a scan, caller's thread */
#define GAP_CENTRAL_DEINIT_STEP_TIMEOUT_MS    (1000U) /* deinit, per step, caller's thread */
#define GAP_CENTRAL_WAIT_POLL_MS              (5U)
#define GATT_CLIENT_DISCOVER_TIMEOUT_MS       (5000U) /* full discovery -> Error */
#define GATT_CLIENT_DISCOVER_CHAR_TIMEOUT_MS  (3000U) /* discover by UUID -> Error */
#define GATT_CLIENT_WRITE_DESC_TIMEOUT_MS     (3000U) /* subscribe -> Error */

/** Initialise. @p conn is the pointer passed to every connection callback. */
void gap_central_init(FuriHalBtCentralConnection* conn);

/** Resolve scan, connection attempt and link (bounded), then pass the callback
 * barrier: when this returns, no application callback runs and none will run.
 * Must be followed by unregistering the dispatcher handler and gap_central_free().
 * Crashes when called from inside a HAL callback.
 */
void gap_central_deinit(void);

/** Release the state of this module. Call after the dispatcher handler is gone. */
void gap_central_free(void);

bool gap_central_start_scan(FuriHalBtCentralScanCallback cb, void* ctx);
bool gap_central_stop_scan(void);

bool gap_central_connect(
    const uint8_t addr[6],
    uint8_t addr_type,
    FuriHalBtCentralEventCallback cb,
    void* ctx);

void gap_central_disconnect(void);

bool gap_central_is_connected(void);
bool gap_central_is_faulted(void);

/** Called by the facade-registered dispatcher handler after gatt_client.
 * Returns BleEventNotAck for events this module doesn't own so the chain
 * continues to the peripheral handlers.
 */
BleEventAckStatus gap_central_handle_event(void* pckt);

/** Deliver a GATT event of the link with sequence number @p link_seq to the
 * application. Dropped when the link is gone, its terminal event was claimed
 * (checked again under the delivery lock, so it never follows the terminal
 * event), or the callback barrier is closed. Called only from the BLE event
 * thread and the deadline worker, never with a lock held.
 */
void gap_central_notify_user(
    uint32_t link_seq,
    FuriHalBtCentralEvent ev,
    uint16_t char_handle,
    const uint8_t* data,
    size_t len);

#ifdef __cplusplus
}
#endif
