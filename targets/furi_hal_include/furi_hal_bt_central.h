/**
 * @file furi_hal_bt_central.h
 * BLE central-mode HAL API
 *
 * Available only when the radio is running the Full BLE stack
 * (`furi_hal_bt_get_radio_stack() == FuriHalBtStackFull`). All functions
 * return false / NULL / 0 when invoked on the Light stack.
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

/** One advertising report returned from an active scan. */
typedef struct {
    uint8_t addr_type;
    uint8_t addr[FURI_HAL_BT_CENTRAL_ADDR_LEN];
    int8_t rssi;
    uint8_t data_len;
    uint8_t data[FURI_HAL_BT_CENTRAL_ADV_DATA_MAX];
} FuriHalBtCentralScanResult;

/** Opaque connection handle. */
typedef struct FuriHalBtCentralConnection FuriHalBtCentralConnection;

/** Event delivered to the caller's connection callback. */
typedef enum {
    FuriHalBtCentralEventConnected,
    FuriHalBtCentralEventDisconnected,
    FuriHalBtCentralEventDiscoveryComplete,
    FuriHalBtCentralEventWriteComplete,
    FuriHalBtCentralEventNotification,
    FuriHalBtCentralEventError,
} FuriHalBtCentralEvent;

/** Scan callback. Fires on every unique advertiser.
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

/** Shut down the central subsystem, disconnecting any active link. */
void furi_hal_bt_central_deinit(void);

/** Report whether central-mode operations are usable on this build.
 *
 * @return true only if the Full BLE stack is running and the subsystem
 *         has been initialized.
 */
bool furi_hal_bt_central_available(void);

/** Start a general discovery scan.
 *
 * One scan callback is fired per unique peer address. Scan runs until
 * furi_hal_bt_central_stop_scan() is called or a connection is created.
 *
 * @param callback  scan callback, must be non-NULL
 * @param context   user context passed to the callback
 *
 * @return true on success
 */
bool furi_hal_bt_central_start_scan(FuriHalBtCentralScanCallback callback, void* context);

/** Stop an active scan.
 *
 * @return true on success (idempotent; returns true if no scan was active)
 */
bool furi_hal_bt_central_stop_scan(void);

/** Open a connection to a peer.
 *
 * Only one connection is supported at a time in this release.
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

/** Tear down a connection.
 *
 * Safe to call at any time; the caller's event callback is invoked with
 * FuriHalBtCentralEventDisconnected once the link is down. After that
 * event the handle becomes invalid.
 *
 * @param conn  connection handle
 */
void furi_hal_bt_central_disconnect(FuriHalBtCentralConnection* conn);

/** Run a full primary-service / characteristic / descriptor discovery.
 *
 * Completes with FuriHalBtCentralEventDiscoveryComplete on the connection
 * callback. After completion, furi_hal_bt_central_find_char() can resolve
 * 128-bit UUIDs to handles.
 *
 * @param conn  connection handle
 *
 * @return true if discovery was started
 */
bool furi_hal_bt_central_gatt_discover(FuriHalBtCentralConnection* conn);

/** Write to a characteristic without response (GATT write command).
 *
 * Fire-and-forget; back-to-back writes within the ATT MTU are permitted.
 *
 * @param conn         connection handle
 * @param char_handle  characteristic value handle
 * @param data         payload
 * @param len          payload length, must fit within the ATT MTU
 *
 * @return true if the command was queued
 */
bool furi_hal_bt_central_gatt_write_nr(
    FuriHalBtCentralConnection* conn,
    uint16_t char_handle,
    const uint8_t* data,
    size_t len);

/** Subscribe to notifications for a characteristic.
 *
 * Writes 0x0001 to the associated CCCD. Subsequent notifications are
 * delivered via FuriHalBtCentralEventNotification on the connection
 * callback.
 *
 * @param conn         connection handle
 * @param char_handle  characteristic value handle
 *
 * @return true if the subscribe command was queued
 */
bool furi_hal_bt_central_gatt_subscribe(
    FuriHalBtCentralConnection* conn,
    uint16_t char_handle);

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
 * @return characteristic value handle, or 0 if not found
 */
uint16_t furi_hal_bt_central_find_char(
    FuriHalBtCentralConnection* conn,
    const uint8_t uuid128[FURI_HAL_BT_CENTRAL_UUID128_LEN]);

#ifdef __cplusplus
}
#endif
