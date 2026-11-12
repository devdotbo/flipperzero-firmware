/**
 * @file furi_hal_bt_central.h
 * BLE central-mode HAL API
 *
 * Available only when the radio is running the Full BLE stack
 * (`furi_hal_bt_get_radio_stack() == FuriHalBtStackFull`). All functions
 * return false / NULL / 0 when invoked on the Light stack.
 *
 * Threads: scan and connection callbacks run on the BLE event thread or on the
 * central's deadline worker thread, never on the thread that made the call.
 * Connection callbacks never run concurrently with each other.
 * Callbacks must not block (post to a queue with timeout 0). From a callback,
 * every function of this API may be called except furi_hal_bt_central_deinit()
 * (crashes) and furi_hal_bt_central_connect() while a scan is running (returns
 * NULL instead of waiting). Calls that wait do so on the caller's thread only:
 * furi_hal_bt_central_connect() during a scan (up to 500 ms) and
 * furi_hal_bt_central_deinit() (bounded, see there).
 *
 * Every state of the central waits for the controller with a deadline. If a
 * deadline passes where the controller state is unknown (cancel, disconnect or
 * scan stop not confirmed), the central becomes faulted: every call fails and
 * furi_hal_bt_central_available() returns false until deinit and init.
 */

#pragma once

#include <furi.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FURI_HAL_BT_CENTRAL_ADDR_LEN        (6)
#define FURI_HAL_BT_CENTRAL_ADV_DATA_MAX    (31)
#define FURI_HAL_BT_CENTRAL_UUID128_LEN     (16)

/** Peer address type as reported by the controller. */
typedef enum {
    FuriHalBtCentralAddrTypePublic = 0,
    FuriHalBtCentralAddrTypeRandom = 1,
} FuriHalBtCentralAddrType;

/** report_type of the last scan callback of a scan: the scan has ended. */
#define FURI_HAL_BT_CENTRAL_SCAN_REPORT_END (0xFFU)

/** char_handle of FuriHalBtCentralEventError: the connect call is over (the
 * attempt failed or timed out, or the central is faulted). It is the terminal
 * event of that connect call. */
#define FURI_HAL_BT_CENTRAL_ERROR_LINK (0U)
/** char_handle of FuriHalBtCentralEventError: a GATT procedure (discovery,
 * subscribe) failed or hit its deadline. The link stays up. */
#define FURI_HAL_BT_CENTRAL_ERROR_GATT (1U)

/** One advertising report returned from an active scan. */
typedef struct {
    uint8_t addr_type;
    uint8_t addr[FURI_HAL_BT_CENTRAL_ADDR_LEN];
    int8_t rssi;
    uint8_t data_len;
    uint8_t data[FURI_HAL_BT_CENTRAL_ADV_DATA_MAX];
    /** Advertising report Event_Type: 0x00 ADV_IND, 0x01 ADV_DIRECT_IND,
     * 0x02 ADV_SCAN_IND, 0x03 ADV_NONCONN_IND, 0x04 SCAN_RSP (scan response).
     * FURI_HAL_BT_CENTRAL_SCAN_REPORT_END marks the end of the scan. */
    uint8_t report_type;
} FuriHalBtCentralScanResult;

/** Opaque connection handle. */
typedef struct FuriHalBtCentralConnection FuriHalBtCentralConnection;

/** Event delivered to the caller's connection callback.
 *
 * Each accepted furi_hal_bt_central_connect() gets exactly one terminal event:
 * FuriHalBtCentralEventDisconnected (link closed, or the attempt was cancelled
 * by furi_hal_bt_central_disconnect()) or FuriHalBtCentralEventError with
 * char_handle FURI_HAL_BT_CENTRAL_ERROR_LINK. No event of that call follows it.
 *
 * The one exception is furi_hal_bt_central_deinit(): it may end a connect call
 * without its terminal event. While deinit runs, the terminal event of a call
 * it resolves can still arrive; once deinit has returned, no event is delivered
 * at all, whether or not the terminal event came. The caller owns the teardown
 * of its per-connection state then.
 */
typedef enum {
    FuriHalBtCentralEventConnected,
    FuriHalBtCentralEventDisconnected,
    FuriHalBtCentralEventDiscoveryComplete,
    /** Not emitted any more: write without response has no completion.
     * Kept for source compatibility. */
    FuriHalBtCentralEventWriteComplete,
    FuriHalBtCentralEventNotification,
    FuriHalBtCentralEventError,
} FuriHalBtCentralEvent;

/** Scan callback. Fires once per (address type, address, report type) seen in
 * this scan, so an advertiser's scan response is reported separately from its
 * advertising report. Merge by address in the application. The dedup table
 * holds 64 entries; when full, the oldest entry is evicted and may be reported
 * again. The last call of every accepted scan has
 * report_type == FURI_HAL_BT_CENTRAL_SCAN_REPORT_END (zero address, no data).
 *
 * @param result   advertising report
 * @param context  user context supplied to furi_hal_bt_central_start_scan()
 */
typedef void (
    *FuriHalBtCentralScanCallback)(const FuriHalBtCentralScanResult* result, void* context);

/** Connection event callback.
 *
 * For FuriHalBtCentralEventNotification, @p data / @p len carry the notified
 * value and @p char_handle identifies the source characteristic. For other
 * events @p data may be NULL and @p len may be 0.
 *
 * @param conn         connection handle
 * @param event        event type
 * @param char_handle  characteristic handle (Notification only, else 0)
 * @param data         event payload
 * @param len          payload length
 * @param context      user context supplied to furi_hal_bt_central_connect()
 */
typedef void (*FuriHalBtCentralEventCallback)(
    FuriHalBtCentralConnection* conn,
    FuriHalBtCentralEvent event,
    uint16_t char_handle,
    const uint8_t* data,
    size_t len,
    void* context);

/** Initialize the central subsystem.
 *
 * No-op on the Light stack.
 */
void furi_hal_bt_central_init(void);

/** Shut down the central subsystem.
 *
 * Stops a scan, cancels a connection attempt and closes a link, waiting up to
 * 1 s for each on the caller's thread; terminal events may be delivered during
 * this. Then it passes the callback barrier: when it returns, no HAL callback
 * is running and none will run later, even if a wait timed out. A connect call
 * whose terminal event was not delivered by then never gets one (see
 * FuriHalBtCentralEvent); the caller owns its teardown. Must not be called from
 * a HAL callback (crashes).
 */
void furi_hal_bt_central_deinit(void);

/** Report whether central-mode operations are usable on this build.
 *
 * @return true only if the Full BLE stack is running, the subsystem
 *         has been initialized and it is not faulted.
 */
bool furi_hal_bt_central_available(void);

/** Start a general discovery scan.
 *
 * Active scanning (scan responses are requested). The controller ends the
 * scan by itself after 10.24 s; furi_hal_bt_central_stop_scan() or
 * furi_hal_bt_central_connect() end it earlier. Either way the scan callback
 * receives a final report with report_type FURI_HAL_BT_CENTRAL_SCAN_REPORT_END.
 * A new scan or connect is accepted only after that.
 *
 * @param callback  scan callback, must be non-NULL
 * @param context   user context passed to the callback
 *
 * @return true on success
 */
bool furi_hal_bt_central_start_scan(FuriHalBtCentralScanCallback callback, void* context);

/** Stop an active scan. The end of the scan is reported to the scan callback.
 *
 * @return true on success (idempotent; returns true if no scan was active)
 */
bool furi_hal_bt_central_stop_scan(void);

/** Open a connection to a peer.
 *
 * Only one connection is supported at a time in this release. Accepted only
 * while no scan, connection attempt or link is pending; a rejected call leaves
 * the callback of the live link untouched. A running scan is stopped first
 * (waits up to 500 ms on the caller's thread; returns NULL if it does not end).
 * The controller keeps trying until furi_hal_bt_central_disconnect() cancels
 * the attempt; after 10 s the HAL cancels it itself (terminal Error).
 * The link keeps the connection parameters the HAL sets here: a peer's request
 * to change them (L2CAP connection parameter update) is rejected.
 *
 * @param addr       peer address (6 bytes, little-endian as the controller expects)
 * @param addr_type  FuriHalBtCentralAddrType value
 * @param callback   connection event callback, must be non-NULL
 * @param context    user context passed to the callback
 *
 * @return connection handle on success, NULL on failure
 */
FuriHalBtCentralConnection* furi_hal_bt_central_connect(
    const uint8_t addr[FURI_HAL_BT_CENTRAL_ADDR_LEN],
    uint8_t addr_type,
    FuriHalBtCentralEventCallback callback,
    void* context);

/** Tear down a connection or cancel a connection attempt.
 *
 * Safe to call at any time and from callbacks; it does not wait. The terminal
 * event follows on the connection callback: FuriHalBtCentralEventDisconnected
 * once the link is down or the attempt was cancelled, or
 * FuriHalBtCentralEventError (FURI_HAL_BT_CENTRAL_ERROR_LINK) if the controller
 * does not confirm in time (the central is then faulted). A connection that
 * completes while the attempt is being cancelled is closed at once and is
 * never reported as Connected. After the terminal event the handle is invalid.
 *
 * @param conn  connection handle
 */
void furi_hal_bt_central_disconnect(FuriHalBtCentralConnection* conn);

/** Run a full primary-service / characteristic / descriptor discovery.
 *
 * Completes with FuriHalBtCentralEventDiscoveryComplete on the connection
 * callback, or FuriHalBtCentralEventError (FURI_HAL_BT_CENTRAL_ERROR_GATT) on
 * failure or after 5 s. After completion, furi_hal_bt_central_find_char() can
 * resolve 128-bit UUIDs to handles. One GATT procedure runs at a time.
 *
 * @param conn  connection handle
 *
 * @return true if discovery was started
 */
bool furi_hal_bt_central_gatt_discover(FuriHalBtCentralConnection* conn);

/** Write to a characteristic without response (GATT write command).
 *
 * Fire-and-forget: there is no completion event and no acknowledgement from
 * the peer. Works with a value handle known from earlier, without discovery.
 *
 * @param conn         connection handle
 * @param char_handle  characteristic value handle
 * @param data         payload
 * @param len          payload length: at most ATT_MTU - 3 (20 bytes unless the
 *                     peer exchanged a larger MTU) and at most 250
 *
 * @return true if the command was queued (false also when the controller's
 *         transmit pool is full; retry later)
 */
bool furi_hal_bt_central_gatt_write_nr(
    FuriHalBtCentralConnection* conn,
    uint16_t char_handle,
    const uint8_t* data,
    size_t len);

/** Subscribe to notifications for a characteristic.
 *
 * Writes 0x0001 to the associated CCCD (found by the full discovery).
 * Subsequent notifications are delivered via FuriHalBtCentralEventNotification
 * on the connection callback. A failed write is reported as
 * FuriHalBtCentralEventError (FURI_HAL_BT_CENTRAL_ERROR_GATT).
 *
 * @param conn         connection handle
 * @param char_handle  characteristic value handle
 *
 * @return true if the subscribe command was queued
 */
bool furi_hal_bt_central_gatt_subscribe(
    FuriHalBtCentralConnection* conn,
    uint16_t char_handle);

/** Discover the characteristic(s) with one 128-bit UUID.
 *
 * Runs one discover-characteristics-by-UUID procedure over the whole handle
 * range. Ends with FuriHalBtCentralEventDiscoveryComplete (then
 * furi_hal_bt_central_find_char() returns the value handle, or 0 if the peer
 * has no such characteristic) or FuriHalBtCentralEventError
 * (FURI_HAL_BT_CENTRAL_ERROR_GATT) on failure or after 3 s. Starting it clears
 * the characteristics found by any earlier discovery, so its own result always
 * fits; afterwards only this UUID resolves, and subscribe needs a new full
 * discovery.
 *
 * @param conn     connection handle
 * @param uuid128  16 bytes of UUID, same byte order as furi_hal_bt_central_find_char()
 *
 * @return true if the procedure was started
 */
bool furi_hal_bt_central_gatt_discover_char(
    FuriHalBtCentralConnection* conn,
    const uint8_t uuid128[FURI_HAL_BT_CENTRAL_UUID128_LEN]);

/** Look up the characteristic value handle for a 128-bit UUID.
 *
 * Must be called after FuriHalBtCentralEventDiscoveryComplete. The UUID
 * bytes are provided in the same byte order as the source specification
 * (human-readable); the implementation handles the on-wire little-endian
 * reversal.
 *
 * @param conn     connection handle
 * @param uuid128  16 bytes of UUID
 *
 * @return characteristic value handle, or 0 if not found or the central is
 *         faulted
 */
uint16_t furi_hal_bt_central_find_char(
    FuriHalBtCentralConnection* conn,
    const uint8_t uuid128[FURI_HAL_BT_CENTRAL_UUID128_LEN]);

#ifdef __cplusplus
}
#endif
