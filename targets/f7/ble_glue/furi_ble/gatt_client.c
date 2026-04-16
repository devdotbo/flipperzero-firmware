/** @file gatt_client.c
 * BLE GATT client: discovery, write, subscribe, notify
 */

/* MVP scope caps:
 *   - single active connection
 *   - up to GATT_CLIENT_MAX_SERVICES (16) primary services
 *   - up to GATT_CLIENT_MAX_CHARS (32) characteristics across all services
 *   - 128-bit UUIDs are stored raw (on-wire little-endian);
 *     16-bit UUIDs are promoted to 128-bit via the Bluetooth Base UUID.
 *   - only CCCD (0x2902) descriptors are tracked.
 */

#include "gatt_client.h"

#include "event_dispatcher.h"

#include "app_common.h"
#include <ble/ble.h>

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_bt.h>

#include <string.h>

#define TAG "BleGattClient"

#define GATT_CLIENT_MAX_SERVICES (16)
#define GATT_CLIENT_MAX_CHARS    (32)

#define GATT_CLIENT_UUID128_LEN  (16)
#define GATT_CLIENT_UUID16_LEN   (2)

/* Attribute-data record sizes reported by the controller. */
#define GATT_CLIENT_SVC_REC_128  (2 + 2 + 16) /* start + end + uuid128 */
#define GATT_CLIENT_SVC_REC_16   (2 + 2 + 2)  /* start + end + uuid16  */
#define GATT_CLIENT_CHAR_REC_128 (2 + 1 + 2 + 16) /* decl + props + value + uuid128 */
#define GATT_CLIENT_CHAR_REC_16  (2 + 1 + 2 + 2)  /* decl + props + value + uuid16  */

#define GATT_CLIENT_CCCD_UUID16 (0x2902)

/* Discovery sub-state. */
typedef enum {
    GattClientDiscIdle,
    GattClientDiscServices,
    GattClientDiscChars,
    GattClientDiscDesc,
    GattClientDiscComplete,
} GattClientDiscState;

typedef struct {
    uint16_t start_handle;
    uint16_t end_handle;
    uint8_t uuid[GATT_CLIENT_UUID128_LEN]; /* on-wire LE bytes */
} GattClientService;

typedef struct {
    uint8_t service_idx;
    uint16_t char_handle; /* handle of the declaration attribute */
    uint8_t properties;
    uint16_t value_handle;
    uint8_t uuid[GATT_CLIENT_UUID128_LEN]; /* on-wire LE bytes */
    uint16_t cccd_handle; /* 0 if none discovered */
} GattClientChar;

typedef struct {
    /* User binding. */
    uint16_t conn_handle;
    FuriHalBtCentralEventCallback cb;
    void* ctx;

    /* Discovery state machine. */
    GattClientDiscState disc_state;
    uint8_t disc_service_idx; /* current service for char / desc iteration */
    uint8_t disc_char_idx;    /* current char for desc iteration */

    /* Discovered topology. */
    uint8_t num_services;
    uint8_t num_chars;
    GattClientService services[GATT_CLIENT_MAX_SERVICES];
    GattClientChar chars[GATT_CLIENT_MAX_CHARS];

    bool initialized;
    bool attached;
} GattClient;

static GattClient gatt_client;

/* Bluetooth Base UUID, on-wire little-endian bytes.
 * Human form: 00000000-0000-1000-8000-00805F9B34FB
 * LE bytes:   FB 34 9B 5F 80 00 00 80 00 10 00 00 00 00 00 00
 * Bytes [12..13] are the 16-bit UUID little-endian (low, high).
 */
static const uint8_t gatt_client_base_uuid_le[GATT_CLIENT_UUID128_LEN] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void gatt_client_promote_uuid16(uint16_t uuid16, uint8_t out[GATT_CLIENT_UUID128_LEN]) {
    memcpy(out, gatt_client_base_uuid_le, GATT_CLIENT_UUID128_LEN);
    out[12] = (uint8_t)(uuid16 & 0xFF);
    out[13] = (uint8_t)((uuid16 >> 8) & 0xFF);
}

static void
    gatt_client_reverse_uuid(const uint8_t in[GATT_CLIENT_UUID128_LEN], uint8_t out[GATT_CLIENT_UUID128_LEN]) {
    for(size_t i = 0; i < GATT_CLIENT_UUID128_LEN; i++) {
        out[i] = in[GATT_CLIENT_UUID128_LEN - 1 - i];
    }
}

static void gatt_client_emit(
    FuriHalBtCentralEvent event,
    uint16_t char_handle,
    const uint8_t* data,
    size_t len) {
    /* Snapshot callback under no lock: the state struct is only mutated
     * on the BLE event worker thread (handle_event) and by the caller
     * thread (attach/discover/...) which never races itself against
     * emit paths the caller initiated synchronously.
     */
    FuriHalBtCentralEventCallback cb = gatt_client.cb;
    void* ctx = gatt_client.ctx;
    if(cb) {
        cb(NULL, event, char_handle, data, len, ctx);
    }
}

/* Find characteristic slot index by value handle. Returns -1 on miss. */
static int gatt_client_char_idx_by_value_handle(uint16_t value_handle) {
    for(uint8_t i = 0; i < gatt_client.num_chars; i++) {
        if(gatt_client.chars[i].value_handle == value_handle) {
            return (int)i;
        }
    }
    return -1;
}

/* Compute the ATT range [start..end] over which to run descriptor
 * discovery for a given characteristic. The range must cover attributes
 * belonging to this characteristic only: from value_handle+1 up to either
 * the handle before the next characteristic in the same service, or the
 * service end handle if this is the last characteristic.
 */
static bool gatt_client_char_desc_range(
    uint8_t char_idx,
    uint16_t* out_start,
    uint16_t* out_end) {
    const GattClientChar* ch = &gatt_client.chars[char_idx];
    const GattClientService* svc = &gatt_client.services[ch->service_idx];

    uint16_t start = ch->value_handle + 1;
    uint16_t end = svc->end_handle;

    /* Look for the next char that belongs to the same service. */
    for(uint8_t i = 0; i < gatt_client.num_chars; i++) {
        if(i == char_idx) continue;
        const GattClientChar* other = &gatt_client.chars[i];
        if(other->service_idx != ch->service_idx) continue;
        if(other->char_handle > ch->char_handle && other->char_handle - 1 < end) {
            end = other->char_handle - 1;
        }
    }

    if(start > end) {
        return false;
    }
    *out_start = start;
    *out_end = end;
    return true;
}

/* ------------------------------------------------------------------ */
/* State machine drivers                                               */
/* ------------------------------------------------------------------ */

static void gatt_client_disc_reset_storage(void) {
    gatt_client.num_services = 0;
    gatt_client.num_chars = 0;
    gatt_client.disc_service_idx = 0;
    gatt_client.disc_char_idx = 0;
    memset(gatt_client.services, 0, sizeof(gatt_client.services));
    memset(gatt_client.chars, 0, sizeof(gatt_client.chars));
}

static void gatt_client_disc_fail(tBleStatus status) {
    FURI_LOG_E(TAG, "Discovery failed: 0x%02X", status);
    gatt_client.disc_state = GattClientDiscIdle;
    gatt_client_emit(FuriHalBtCentralEventError, 0, NULL, 0);
}

/* Issue char-discovery for services[disc_service_idx]. */
static bool gatt_client_disc_start_chars_for_current_service(void) {
    if(gatt_client.disc_service_idx >= gatt_client.num_services) {
        return false;
    }
    const GattClientService* svc = &gatt_client.services[gatt_client.disc_service_idx];
    furi_hal_bt_lock_core2();
    tBleStatus status = aci_gatt_disc_all_char_of_service(
        gatt_client.conn_handle, svc->start_handle, svc->end_handle);
    furi_hal_bt_unlock_core2();
    if(status != BLE_STATUS_SUCCESS) {
        FURI_LOG_E(
            TAG, "disc_all_char_of_service failed for svc %u: 0x%02X",
            gatt_client.disc_service_idx, status);
        gatt_client_disc_fail(status);
        return false;
    }
    return true;
}

/* Advance descriptor discovery: issue disc_all_char_desc for the next
 * characteristic that has a usable descriptor range. Skips characteristics
 * with empty ranges. Returns true if a command was queued, false if the
 * descriptor phase is complete (no more candidates).
 */
static bool gatt_client_disc_issue_next_desc_cmd(void) {
    while(gatt_client.disc_char_idx < gatt_client.num_chars) {
        uint16_t start = 0;
        uint16_t end = 0;
        if(gatt_client_char_desc_range(gatt_client.disc_char_idx, &start, &end)) {
            furi_hal_bt_lock_core2();
            tBleStatus status =
                aci_gatt_disc_all_char_desc(gatt_client.conn_handle, start, end);
            furi_hal_bt_unlock_core2();
            if(status != BLE_STATUS_SUCCESS) {
                FURI_LOG_E(
                    TAG, "disc_all_char_desc failed for char %u: 0x%02X",
                    gatt_client.disc_char_idx, status);
                gatt_client_disc_fail(status);
                return false;
            }
            return true;
        }
        /* Empty range: no descriptors possible for this characteristic. */
        gatt_client.disc_char_idx++;
    }
    return false;
}

static void gatt_client_disc_finish(void) {
    gatt_client.disc_state = GattClientDiscComplete;
    FURI_LOG_I(
        TAG,
        "Discovery complete: %u services, %u characteristics",
        gatt_client.num_services,
        gatt_client.num_chars);
    gatt_client.disc_state = GattClientDiscIdle;
    gatt_client_emit(FuriHalBtCentralEventDiscoveryComplete, 0, NULL, 0);
}

/* Called on ACI_GATT_PROC_COMPLETE_VSEVT_CODE. */
static void gatt_client_on_proc_complete(uint8_t error_code) {
    if(error_code != 0) {
        gatt_client_disc_fail(error_code);
        return;
    }

    switch(gatt_client.disc_state) {
    case GattClientDiscServices:
        FURI_LOG_I(
            TAG, "Services discovered: %u", gatt_client.num_services);
        if(gatt_client.num_services == 0) {
            /* Nothing more to do. */
            gatt_client_disc_finish();
            return;
        }
        gatt_client.disc_state = GattClientDiscChars;
        gatt_client.disc_service_idx = 0;
        if(!gatt_client_disc_start_chars_for_current_service()) {
            return; /* gatt_client_disc_fail() already emitted */
        }
        break;

    case GattClientDiscChars:
        gatt_client.disc_service_idx++;
        if(gatt_client.disc_service_idx < gatt_client.num_services) {
            if(!gatt_client_disc_start_chars_for_current_service()) {
                return;
            }
        } else {
            FURI_LOG_I(TAG, "Characteristics discovered: %u", gatt_client.num_chars);
            if(gatt_client.num_chars == 0) {
                gatt_client_disc_finish();
                return;
            }
            gatt_client.disc_state = GattClientDiscDesc;
            gatt_client.disc_char_idx = 0;
            if(!gatt_client_disc_issue_next_desc_cmd()) {
                /* No descriptor candidates at all -> done. */
                gatt_client_disc_finish();
                return;
            }
        }
        break;

    case GattClientDiscDesc:
        gatt_client.disc_char_idx++;
        if(!gatt_client_disc_issue_next_desc_cmd()) {
            gatt_client_disc_finish();
            return;
        }
        break;

    default:
        /* Unexpected proc-complete; ignore. */
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Event payload parsers                                               */
/* ------------------------------------------------------------------ */

static void gatt_client_parse_service_records(
    const uint8_t* data,
    uint8_t total_len,
    uint8_t record_len) {
    if(record_len != GATT_CLIENT_SVC_REC_128 && record_len != GATT_CLIENT_SVC_REC_16) {
        FURI_LOG_E(TAG, "Unexpected service record len %u", record_len);
        return;
    }
    const uint8_t* p = data;
    const uint8_t* end = data + total_len;
    while(p + record_len <= end) {
        if(gatt_client.num_services >= GATT_CLIENT_MAX_SERVICES) {
            FURI_LOG_W(TAG, "Service cap reached (%u), dropping rest", GATT_CLIENT_MAX_SERVICES);
            return;
        }
        GattClientService* svc = &gatt_client.services[gatt_client.num_services++];
        svc->start_handle = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
        svc->end_handle = (uint16_t)p[2] | ((uint16_t)p[3] << 8);
        if(record_len == GATT_CLIENT_SVC_REC_128) {
            memcpy(svc->uuid, p + 4, GATT_CLIENT_UUID128_LEN);
        } else {
            uint16_t uuid16 = (uint16_t)p[4] | ((uint16_t)p[5] << 8);
            gatt_client_promote_uuid16(uuid16, svc->uuid);
        }
        p += record_len;
    }
}

static void gatt_client_parse_char_records(
    const uint8_t* data,
    uint8_t total_len,
    uint8_t record_len) {
    if(record_len != GATT_CLIENT_CHAR_REC_128 && record_len != GATT_CLIENT_CHAR_REC_16) {
        FURI_LOG_E(TAG, "Unexpected char record len %u", record_len);
        return;
    }
    const uint8_t* p = data;
    const uint8_t* end = data + total_len;
    while(p + record_len <= end) {
        if(gatt_client.num_chars >= GATT_CLIENT_MAX_CHARS) {
            FURI_LOG_W(TAG, "Char cap reached (%u), dropping rest", GATT_CLIENT_MAX_CHARS);
            return;
        }
        GattClientChar* ch = &gatt_client.chars[gatt_client.num_chars++];
        ch->service_idx = gatt_client.disc_service_idx;
        ch->char_handle = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
        ch->properties = p[2];
        ch->value_handle = (uint16_t)p[3] | ((uint16_t)p[4] << 8);
        if(record_len == GATT_CLIENT_CHAR_REC_128) {
            memcpy(ch->uuid, p + 5, GATT_CLIENT_UUID128_LEN);
        } else {
            uint16_t uuid16 = (uint16_t)p[5] | ((uint16_t)p[6] << 8);
            gatt_client_promote_uuid16(uuid16, ch->uuid);
        }
        ch->cccd_handle = 0;
        p += record_len;
    }
}

static void gatt_client_parse_desc_records(const aci_att_find_info_resp_event_rp0* ev) {
    /* Format: 0x01 = 16-bit UUIDs, 0x02 = 128-bit UUIDs. */
    const uint8_t format = ev->Format;
    const uint8_t total_len = ev->Event_Data_Length;
    const uint8_t* data = ev->Handle_UUID_Pair;

    uint8_t pair_len;
    if(format == 0x01) {
        pair_len = 2 + GATT_CLIENT_UUID16_LEN;
    } else if(format == 0x02) {
        pair_len = 2 + GATT_CLIENT_UUID128_LEN;
    } else {
        FURI_LOG_E(TAG, "Unexpected find_info format %u", format);
        return;
    }

    if(gatt_client.disc_char_idx >= gatt_client.num_chars) {
        return;
    }
    GattClientChar* ch = &gatt_client.chars[gatt_client.disc_char_idx];

    const uint8_t* p = data;
    const uint8_t* end = data + total_len;
    while(p + pair_len <= end) {
        uint16_t handle = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
        if(format == 0x01) {
            uint16_t uuid16 = (uint16_t)p[2] | ((uint16_t)p[3] << 8);
            if(uuid16 == GATT_CLIENT_CCCD_UUID16 && ch->cccd_handle == 0) {
                ch->cccd_handle = handle;
            }
        }
        /* 128-bit descriptors are not tracked (no CCCD is 128-bit). */
        p += pair_len;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void gatt_client_init(void) {
    memset(&gatt_client, 0, sizeof(gatt_client));
    gatt_client.disc_state = GattClientDiscIdle;
    gatt_client.initialized = true;
    FURI_LOG_I(TAG, "init");
}

void gatt_client_deinit(void) {
    if(!gatt_client.initialized) {
        return;
    }
    memset(&gatt_client, 0, sizeof(gatt_client));
    FURI_LOG_I(TAG, "deinit");
}

void gatt_client_reset(void) {
    if(!gatt_client.initialized) {
        return;
    }
    FURI_LOG_I(TAG, "reset");
    gatt_client.conn_handle = 0;
    gatt_client.cb = NULL;
    gatt_client.ctx = NULL;
    gatt_client.attached = false;
    gatt_client.disc_state = GattClientDiscIdle;
    gatt_client_disc_reset_storage();
}

void gatt_client_attach(
    uint16_t conn_handle,
    FuriHalBtCentralEventCallback cb,
    void* ctx) {
    furi_check(gatt_client.initialized);
    gatt_client.conn_handle = conn_handle;
    gatt_client.cb = cb;
    gatt_client.ctx = ctx;
    gatt_client.attached = (cb != NULL);
    gatt_client.disc_state = GattClientDiscIdle;
    gatt_client_disc_reset_storage();
    FURI_LOG_I(TAG, "attach conn_handle=0x%04X", conn_handle);
}

bool gatt_client_discover(void) {
    furi_check(gatt_client.initialized);
    if(!gatt_client.attached) {
        FURI_LOG_E(TAG, "discover: not attached");
        return false;
    }
    if(gatt_client.disc_state != GattClientDiscIdle) {
        FURI_LOG_E(TAG, "discover: busy (state=%u)", gatt_client.disc_state);
        return false;
    }

    gatt_client_disc_reset_storage();
    gatt_client.disc_state = GattClientDiscServices;

    furi_hal_bt_lock_core2();
    tBleStatus status = aci_gatt_disc_all_primary_services(gatt_client.conn_handle);
    furi_hal_bt_unlock_core2();
    if(status != BLE_STATUS_SUCCESS) {
        FURI_LOG_E(TAG, "disc_all_primary_services failed: 0x%02X", status);
        gatt_client.disc_state = GattClientDiscIdle;
        return false;
    }
    FURI_LOG_I(TAG, "discover: services started");
    return true;
}

bool gatt_client_write_nr(uint16_t char_value_handle, const uint8_t* data, size_t len) {
    furi_check(gatt_client.initialized);
    if(!gatt_client.attached) {
        FURI_LOG_E(TAG, "write_nr: not attached");
        return false;
    }
    if(len > UINT8_MAX) {
        FURI_LOG_E(TAG, "write_nr: len %u exceeds ATT payload max", (unsigned)len);
        return false;
    }

    furi_hal_bt_lock_core2();
    tBleStatus status = aci_gatt_write_without_resp(
        gatt_client.conn_handle, char_value_handle, (uint8_t)len, data);
    furi_hal_bt_unlock_core2();

    if(status != BLE_STATUS_SUCCESS) {
        FURI_LOG_E(
            TAG, "write_without_resp handle=0x%04X failed: 0x%02X", char_value_handle, status);
        return false;
    }

    /* Fire-and-forget at ACI level: synthesize a WriteComplete on the
     * caller's thread so the user layer has a uniform event stream.
     */
    gatt_client_emit(FuriHalBtCentralEventWriteComplete, char_value_handle, NULL, 0);
    return true;
}

bool gatt_client_subscribe(uint16_t char_value_handle) {
    furi_check(gatt_client.initialized);
    if(!gatt_client.attached) {
        FURI_LOG_E(TAG, "subscribe: not attached");
        return false;
    }

    int idx = gatt_client_char_idx_by_value_handle(char_value_handle);
    if(idx < 0) {
        FURI_LOG_E(TAG, "subscribe: unknown handle 0x%04X", char_value_handle);
        return false;
    }
    uint16_t cccd_handle = gatt_client.chars[idx].cccd_handle;
    if(cccd_handle == 0) {
        FURI_LOG_E(TAG, "subscribe: no CCCD for handle 0x%04X", char_value_handle);
        return false;
    }

    const uint8_t cccd_value[2] = {0x01, 0x00};
    furi_hal_bt_lock_core2();
    tBleStatus status = aci_gatt_write_char_desc(
        gatt_client.conn_handle, cccd_handle, sizeof(cccd_value), cccd_value);
    furi_hal_bt_unlock_core2();

    if(status != BLE_STATUS_SUCCESS) {
        FURI_LOG_E(
            TAG,
            "write_char_desc cccd=0x%04X failed: 0x%02X",
            cccd_handle,
            status);
        return false;
    }
    FURI_LOG_I(TAG, "subscribe: handle=0x%04X cccd=0x%04X", char_value_handle, cccd_handle);
    return true;
}

uint16_t gatt_client_find_char(const uint8_t uuid128_human[16]) {
    furi_check(gatt_client.initialized);
    furi_check(uuid128_human);

    uint8_t needle_wire[GATT_CLIENT_UUID128_LEN];
    gatt_client_reverse_uuid(uuid128_human, needle_wire);

    for(uint8_t i = 0; i < gatt_client.num_chars; i++) {
        if(memcmp(gatt_client.chars[i].uuid, needle_wire, GATT_CLIENT_UUID128_LEN) == 0) {
            return gatt_client.chars[i].value_handle;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Dispatcher entry                                                    */
/* ------------------------------------------------------------------ */

BleEventAckStatus gatt_client_handle_event(void* pckt) {
    furi_check(pckt);
    if(!gatt_client.initialized) {
        return BleEventNotAck;
    }

    hci_event_pckt* event_pckt = (hci_event_pckt*)((hci_uart_pckt*)pckt)->data;
    if(event_pckt->evt != HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE) {
        return BleEventNotAck;
    }

    evt_blecore_aci* blue_evt = (evt_blecore_aci*)event_pckt->data;
    switch(blue_evt->ecode) {
    case ACI_ATT_READ_BY_GROUP_TYPE_RESP_VSEVT_CODE: {
        if(gatt_client.disc_state != GattClientDiscServices) {
            return BleEventNotAck;
        }
        aci_att_read_by_group_type_resp_event_rp0* ev =
            (aci_att_read_by_group_type_resp_event_rp0*)blue_evt->data;
        if(ev->Connection_Handle != gatt_client.conn_handle) {
            return BleEventNotAck;
        }
        gatt_client_parse_service_records(
            ev->Attribute_Data_List, ev->Data_Length, ev->Attribute_Data_Length);
        return BleEventAckFlowEnable;
    }

    case ACI_ATT_READ_BY_TYPE_RESP_VSEVT_CODE: {
        if(gatt_client.disc_state != GattClientDiscChars) {
            return BleEventNotAck;
        }
        aci_att_read_by_type_resp_event_rp0* ev =
            (aci_att_read_by_type_resp_event_rp0*)blue_evt->data;
        if(ev->Connection_Handle != gatt_client.conn_handle) {
            return BleEventNotAck;
        }
        gatt_client_parse_char_records(
            ev->Handle_Value_Pair_Data, ev->Data_Length, ev->Handle_Value_Pair_Length);
        return BleEventAckFlowEnable;
    }

    case ACI_ATT_FIND_INFO_RESP_VSEVT_CODE: {
        if(gatt_client.disc_state != GattClientDiscDesc) {
            return BleEventNotAck;
        }
        aci_att_find_info_resp_event_rp0* ev =
            (aci_att_find_info_resp_event_rp0*)blue_evt->data;
        if(ev->Connection_Handle != gatt_client.conn_handle) {
            return BleEventNotAck;
        }
        gatt_client_parse_desc_records(ev);
        return BleEventAckFlowEnable;
    }

    case ACI_GATT_PROC_COMPLETE_VSEVT_CODE: {
        aci_gatt_proc_complete_event_rp0* ev =
            (aci_gatt_proc_complete_event_rp0*)blue_evt->data;
        if(ev->Connection_Handle != gatt_client.conn_handle) {
            return BleEventNotAck;
        }
        if(gatt_client.disc_state == GattClientDiscIdle) {
            /* Not our procedure; let other handlers see it. */
            return BleEventNotAck;
        }
        gatt_client_on_proc_complete(ev->Error_Code);
        return BleEventAckFlowEnable;
    }

    case ACI_GATT_NOTIFICATION_VSEVT_CODE: {
        aci_gatt_notification_event_rp0* ev =
            (aci_gatt_notification_event_rp0*)blue_evt->data;
        if(ev->Connection_Handle != gatt_client.conn_handle) {
            return BleEventNotAck;
        }
        gatt_client_emit(
            FuriHalBtCentralEventNotification,
            ev->Attribute_Handle,
            ev->Attribute_Value,
            ev->Attribute_Value_Length);
        return BleEventAckFlowEnable;
    }

    default:
        return BleEventNotAck;
    }
}
