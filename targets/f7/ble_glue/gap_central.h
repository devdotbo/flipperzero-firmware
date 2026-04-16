/** @file gap_central.h
 * BLE central GAP: scan + connect/disconnect + event dispatch (internal)
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

void gap_central_init(void);
void gap_central_deinit(void);

bool gap_central_start_scan(FuriHalBtCentralScanCallback cb, void* ctx);
bool gap_central_stop_scan(void);

bool gap_central_connect(
    const uint8_t addr[6],
    uint8_t addr_type,
    FuriHalBtCentralEventCallback cb,
    void* ctx);

bool gap_central_disconnect(void);

bool gap_central_is_connected(void);
uint16_t gap_central_get_conn_handle(void);
void gap_central_get_peer_addr(uint8_t out_addr[6], uint8_t* out_addr_type);

/* Called by facade-registered dispatcher handler.
 * Returns BleEventNotAck for events this module doesn't own so the chain
 * continues to gatt_client and peripheral handlers.
 */
BleEventAckStatus gap_central_handle_event(void* pckt);

/* Forward a central event to the user's FuriHalBtCentralEventCallback.
 * Used by the facade (and gatt_client via the facade) to deliver events
 * such as FuriHalBtCentralEventDiscoveryComplete / EventWriteComplete /
 * EventNotification that originate outside this module.
 */
void gap_central_notify_user(
    FuriHalBtCentralEvent ev,
    uint16_t char_handle,
    const uint8_t* data,
    size_t len);

#ifdef __cplusplus
}
#endif
