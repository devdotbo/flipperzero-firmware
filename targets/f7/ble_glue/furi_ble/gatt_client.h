/** @file gatt_client.h
 * BLE GATT client: discovery, write, subscribe, notify (internal)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <furi_hal_bt_central.h>

#include "event_dispatcher.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One-time module init. */
void gatt_client_init(void);

/** One-time module teardown. */
void gatt_client_deinit(void);

/** Clear discovery / attachment state.
 *
 * Called from gap_central on disconnect via extern forward declaration.
 * Idempotent.
 */
void gatt_client_reset(void);

/** Bind a user callback + context to the current connection.
 *
 * Must be called by the facade after the connection completes and before
 * any gatt_client_discover / _write_nr / _subscribe / _find_char call.
 *
 * @param conn_handle  controller connection handle
 * @param cb           user event callback (may be NULL to detach)
 * @param ctx          user context forwarded to @p cb
 */
void gatt_client_attach(
    uint16_t conn_handle,
    FuriHalBtCentralEventCallback cb,
    void* ctx);

/** Start a full primary-service / characteristic / descriptor discovery.
 *
 * Uses the connection handle bound by gatt_client_attach(). Completes with
 * FuriHalBtCentralEventDiscoveryComplete on the attached callback.
 *
 * @return true if the first ACI step was queued
 */
bool gatt_client_discover(void);

/** Write to a characteristic without response.
 *
 * Synchronous ACI command. On success, the user callback is invoked inline
 * on the caller's thread with FuriHalBtCentralEventWriteComplete.
 *
 * @param char_value_handle  characteristic value handle
 * @param data               payload (may be NULL if @p len is 0)
 * @param len                payload length, must fit within ATT MTU
 *
 * @return true on ACI success
 */
bool gatt_client_write_nr(uint16_t char_value_handle, const uint8_t* data, size_t len);

/** Subscribe to notifications for a characteristic.
 *
 * Writes {0x01, 0x00} to the CCCD discovered for @p char_value_handle.
 * Requires that discovery has completed and the CCCD was found.
 *
 * @param char_value_handle  characteristic value handle
 *
 * @return true on ACI success
 */
bool gatt_client_subscribe(uint16_t char_value_handle);

/** Resolve a 128-bit UUID to a characteristic value handle.
 *
 * UUID is provided in human-readable byte order (same order as source
 * specification). The implementation reverses to on-wire LE internally.
 *
 * @param uuid128_human  16 bytes, human-readable order
 *
 * @return value handle on hit, 0 on miss
 */
uint16_t gatt_client_find_char(const uint8_t uuid128_human[16]);

/** Process one raw BLE packet from the dispatcher.
 *
 * Returns BleEventNotAck for events this module does not own so the chain
 * continues to gap_central and any peripheral handlers.
 *
 * @param pckt  raw hci_uart_pckt*
 *
 * @return ack / flow status
 */
BleEventAckStatus gatt_client_handle_event(void* pckt);

#ifdef __cplusplus
}
#endif
