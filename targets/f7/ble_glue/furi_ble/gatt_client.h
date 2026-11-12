/** @file gatt_client.h
 * BLE GATT client: discovery, write, subscribe, notify (internal)
 *
 * State is protected by an internal mutex. Application events are delivered
 * through gap_central_notify_user() after the mutex is released, so they carry
 * the facade's connection pointer and pass the central's callback barrier.
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

/** One-time module teardown. Call after the dispatcher handler is removed. */
void gatt_client_deinit(void);

/** Detach from the link and clear discovery state. Called on link loss. Idempotent. */
void gatt_client_reset(void);

/** Bind to a new link.
 *
 * @param conn_handle  controller connection handle
 * @param link_seq     central's sequence number of the connect call
 */
void gatt_client_attach(uint16_t conn_handle, uint32_t link_seq);

/** Start a full primary-service / characteristic / descriptor discovery.
 *
 * Ends with FuriHalBtCentralEventDiscoveryComplete, or with
 * FuriHalBtCentralEventError (FURI_HAL_BT_CENTRAL_ERROR_GATT) on failure or
 * after GATT_CLIENT_DISCOVER_TIMEOUT_MS.
 *
 * @return true if the first ACI step was queued
 */
bool gatt_client_discover(void);

/** Discover the characteristics with one 128-bit UUID over the whole handle range.
 *
 * Ends like gatt_client_discover(), deadline GATT_CLIENT_DISCOVER_CHAR_TIMEOUT_MS.
 * When the procedure starts, it replaces the table that gatt_client_find_char()
 * searches: earlier entries, full or targeted, are dropped, and subscribe()
 * needs a new full discovery. A call that returns false leaves the table as it
 * was.
 *
 * @param uuid128_human  16 bytes, human-readable order
 *
 * @return true if the ACI command was queued
 */
bool gatt_client_discover_char(const uint8_t uuid128_human[16]);

/** Write to a characteristic without response.
 *
 * No completion event exists for this command. @p len must not exceed
 * ATT_MTU - 3 (20 bytes unless the MTU was exchanged) and 250.
 *
 * @return true on ACI success
 */
bool gatt_client_write_nr(uint16_t char_value_handle, const uint8_t* data, size_t len);

/** Subscribe to notifications for a characteristic.
 *
 * Writes {0x01, 0x00} to the CCCD found by a full discovery. Failure after the
 * command was queued is reported as FuriHalBtCentralEventError
 * (FURI_HAL_BT_CENTRAL_ERROR_GATT).
 *
 * @return true on ACI success
 */
bool gatt_client_subscribe(uint16_t char_value_handle);

/** Resolve a 128-bit UUID (human-readable order) to a value handle, 0 on miss. */
uint16_t gatt_client_find_char(const uint8_t uuid128_human[16]);

/** Deadline check, called periodically by the central's deadline worker. */
void gatt_client_tick(void);

/** Process one raw BLE packet from the dispatcher (BLE event thread).
 *
 * Returns BleEventNotAck for events this module does not own.
 */
BleEventAckStatus gatt_client_handle_event(void* pckt);

#ifdef __cplusplus
}
#endif
