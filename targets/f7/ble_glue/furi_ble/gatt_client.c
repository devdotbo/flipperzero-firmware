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
 *
 * Threads and locking: the state below is protected by gatt_client.mutex. It is
 * taken by application threads (API calls), the BLE event thread (responses) and
 * the central's deadline worker thread, held across single ACI commands, and
 * never held while calling gap_central or the application. One GATT procedure
 * runs at a time; each has a deadline. A procedure abandoned at its deadline
 * leaves a procedure-complete to come; no new procedure starts before it is
 * consumed, so it cannot be mistaken for the completion of a later one.
 */

#include "gatt_client.h"

#include "event_dispatcher.h"
#include "../gap_central.h"

#include "app_common.h"
#include <ble/ble.h>

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_bt.h>

#include <string.h>

#define TAG "BleGattClient"

#define GATT_CLIENT_MAX_SERVICES (16)
#define GATT_CLIENT_MAX_CHARS    (32)

#define GATT_CLIENT_UUID128_LEN (16)
#define GATT_CLIENT_UUID16_LEN  (2)

/* Attribute-data record sizes reported by the controller. */
#define GATT_CLIENT_SVC_REC_128  (2 + 2 + 16) /* start + end + uuid128 */
#define GATT_CLIENT_SVC_REC_16   (2 + 2 + 2) /* start + end + uuid16  */
#define GATT_CLIENT_CHAR_REC_128 (2 + 1 + 2 + 16) /* decl + props + value + uuid128 */
#define GATT_CLIENT_CHAR_REC_16  (2 + 1 + 2 + 2) /* decl + props + value + uuid16  */
#define GATT_CLIENT_CHAR_DECL_128 (1 + 2 + 16) /* props + value + uuid128 */

#define GATT_CLIENT_CCCD_UUID16 (0x2902)

/* ATT payload limits: ATT_MTU - 3, and the STM32WB command limit
 * (BLE_CMD_MAX_PARAM_LEN - 5) for write without response. */
#define GATT_CLIENT_ATT_HEADER    (3U)
#define GATT_CLIENT_WRITE_CMD_MAX (250U)
#define GATT_CLIENT_ATT_ERR_ATTR_NOT_FOUND (0x0AU)
#define GATT_CLIENT_STALE_MAX     (2U)

typedef enum {
    GattClientProcIdle,
    GattClientProcServices,
    GattClientProcChars,
    GattClientProcDesc,
    GattClientProcCharByUuid,
    GattClientProcWriteDesc,
} GattClientProc;

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
    FuriMutex* mutex;

    /* Link binding. */
    bool attached;
    uint16_t conn_handle;
    uint32_t link_seq;
    uint16_t att_mtu;

    /* Running procedure. */
    GattClientProc proc;
    uint32_t deadline;
    uint8_t stale_procs; /* abandoned procedures whose procedure-complete is still due */
    uint8_t disc_service_idx; /* current service for char iteration */
    uint8_t disc_char_idx; /* current char for desc iteration */
    uint8_t target_uuid[GATT_CLIENT_UUID128_LEN]; /* discover-by-UUID target, wire order */

    /* Discovered topology. */
    uint8_t num_services;
    uint8_t num_chars;
    GattClientService services[GATT_CLIENT_MAX_SERVICES];
    GattClientChar chars[GATT_CLIENT_MAX_CHARS];

    bool initialized;
} GattClient;

static GattClient gatt_client;

/* Event decided under the mutex, delivered after it. */
typedef struct {
    bool valid;
    uint32_t link_seq;
    FuriHalBtCentralEvent event;
    uint16_t char_handle;
    const uint8_t* data;
    size_t len;
} GattClientEmit;

/* Bluetooth Base UUID, on-wire little-endian bytes.
 * Human form: 00000000-0000-1000-8000-00805F9B34FB
 * Bytes [12..13] are the 16-bit UUID little-endian (low, high).
 */
static const uint8_t gatt_client_base_uuid_le[GATT_CLIENT_UUID128_LEN] = {
    0xFB,
    0x34,
    0x9B,
    0x5F,
    0x80,
    0x00,
    0x00,
    0x80,
    0x00,
    0x10,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static inline void gatt_client_lock(void) {
    furi_check(furi_mutex_acquire(gatt_client.mutex, FuriWaitForever) == FuriStatusOk);
}

static inline void gatt_client_unlock(void) {
    furi_check(furi_mutex_release(gatt_client.mutex) == FuriStatusOk);
}

static void gatt_client_promote_uuid16(uint16_t uuid16, uint8_t out[GATT_CLIENT_UUID128_LEN]) {
    memcpy(out, gatt_client_base_uuid_le, GATT_CLIENT_UUID128_LEN);
    out[12] = (uint8_t)(uuid16 & 0xFF);
    out[13] = (uint8_t)((uuid16 >> 8) & 0xFF);
}

static void gatt_client_reverse_uuid(
    const uint8_t in[GATT_CLIENT_UUID128_LEN],
    uint8_t out[GATT_CLIENT_UUID128_LEN]) {
    for(size_t i = 0; i < GATT_CLIENT_UUID128_LEN; i++) {
        out[i] = in[GATT_CLIENT_UUID128_LEN - 1 - i];
    }
}

static inline uint16_t gatt_client_read_u16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/* Caller holds the mutex. */
static void gatt_client_prepare(
    GattClientEmit* e,
    FuriHalBtCentralEvent event,
    uint16_t char_handle,
    const uint8_t* data,
    size_t len) {
    e->valid = true;
    e->link_seq = gatt_client.link_seq;
    e->event = event;
    e->char_handle = char_handle;
    e->data = data;
    e->len = len;
}

/* Called WITHOUT the mutex. */
static void gatt_client_deliver(const GattClientEmit* e) {
    if(e->valid) {
        gap_central_notify_user(e->link_seq, e->event, e->char_handle, e->data, e->len);
    }
}

/* Caller holds the mutex. */
static void gatt_client_start_proc(GattClientProc proc, uint32_t timeout_ms) {
    gatt_client.proc = proc;
    gatt_client.deadline = furi_get_tick() + furi_ms_to_ticks(timeout_ms);
}

/* Caller holds the mutex. A new procedure may start only when none runs and no
 * abandoned one can still complete. */
static bool gatt_client_can_start(const char* what) {
    if(!gatt_client.attached) {
        FURI_LOG_E(TAG, "%s: not attached", what);
        return false;
    }
    if(gatt_client.proc != GattClientProcIdle || gatt_client.stale_procs > 0) {
        FURI_LOG_E(
            TAG, "%s: busy (proc=%u stale=%u)", what, gatt_client.proc, gatt_client.stale_procs);
        return false;
    }
    return true;
}

static int gatt_client_char_idx_by_value_handle(uint16_t value_handle) {
    for(uint8_t i = 0; i < gatt_client.num_chars; i++) {
        if(gatt_client.chars[i].value_handle == value_handle) {
            return (int)i;
        }
    }
    return -1;
}

/* Compute the ATT range [start..end] over which to run descriptor
 * discovery for a given characteristic: from value_handle+1 up to either
 * the handle before the next characteristic in the same service, or the
 * service end handle if this is the last characteristic.
 */
static bool gatt_client_char_desc_range(uint8_t char_idx, uint16_t* out_start, uint16_t* out_end) {
    const GattClientChar* ch = &gatt_client.chars[char_idx];
    const GattClientService* svc = &gatt_client.services[ch->service_idx];

    uint16_t start = ch->value_handle + 1;
    uint16_t end = svc->end_handle;

    for(uint8_t i = 0; i < gatt_client.num_chars; i++) {
        if(i == char_idx) continue;
        const GattClientChar* other = &gatt_client.chars[i];
        if(other->service_idx != ch->service_idx) continue;
        if(other->char_handle > ch->char_handle && other->char_handle - 1 < end) {
            end = other->char_handle - 1;
        }
    }

    if(start == 0 || start > end) {
        return false;
    }
    *out_start = start;
    *out_end = end;
    return true;
}

static void gatt_client_disc_reset_storage(void) {
    gatt_client.num_services = 0;
    gatt_client.num_chars = 0;
    gatt_client.disc_service_idx = 0;
    gatt_client.disc_char_idx = 0;
    memset(gatt_client.services, 0, sizeof(gatt_client.services));
    memset(gatt_client.chars, 0, sizeof(gatt_client.chars));
}

/* ------------------------------------------------------------------ */
/* Procedure state machine (caller holds the mutex)                    */
/* ------------------------------------------------------------------ */

static void gatt_client_proc_fail(GattClientEmit* e, uint8_t status) {
    FURI_LOG_E(TAG, "Procedure %u failed: 0x%02X", gatt_client.proc, status);
    gatt_client.proc = GattClientProcIdle;
    gatt_client_prepare(e, FuriHalBtCentralEventError, FURI_HAL_BT_CENTRAL_ERROR_GATT, NULL, 0);
}

static void gatt_client_disc_finish(GattClientEmit* e) {
    FURI_LOG_I(
        TAG,
        "Discovery complete: %u services, %u characteristics",
        gatt_client.num_services,
        gatt_client.num_chars);
    gatt_client.proc = GattClientProcIdle;
    gatt_client_prepare(e, FuriHalBtCentralEventDiscoveryComplete, 0, NULL, 0);
}

/* Issue char-discovery for services[disc_service_idx]. */
static bool gatt_client_disc_start_chars_for_current_service(GattClientEmit* e) {
    const GattClientService* svc = &gatt_client.services[gatt_client.disc_service_idx];
    tBleStatus status = aci_gatt_disc_all_char_of_service(
        gatt_client.conn_handle, svc->start_handle, svc->end_handle);
    if(status != BLE_STATUS_SUCCESS) {
        gatt_client_proc_fail(e, status);
        return false;
    }
    return true;
}

/* Issue disc_all_char_desc for the next characteristic with a usable range.
 * Returns true if a command was queued, false if the descriptor phase is done. */
static bool gatt_client_disc_issue_next_desc_cmd(void) {
    while(gatt_client.disc_char_idx < gatt_client.num_chars) {
        uint16_t start = 0;
        uint16_t end = 0;
        if(gatt_client_char_desc_range(gatt_client.disc_char_idx, &start, &end)) {
            tBleStatus status = aci_gatt_disc_all_char_desc(gatt_client.conn_handle, start, end);
            if(status == BLE_STATUS_SUCCESS) {
                return true;
            }
            /* Best effort: CCCDs are only needed for subscribe(). */
            FURI_LOG_W(
                TAG,
                "disc_all_char_desc skipped for char %u: 0x%02X",
                gatt_client.disc_char_idx,
                status);
        }
        gatt_client.disc_char_idx++;
    }
    return false;
}

/* ACI_GATT_PROC_COMPLETE for the running procedure. */
static void gatt_client_on_proc_complete(GattClientEmit* e, uint8_t error_code) {
    /* "Attribute not found" ends a discovery normally (nothing (more) found). */
    if(error_code != 0 && error_code != GATT_CLIENT_ATT_ERR_ATTR_NOT_FOUND) {
        gatt_client_proc_fail(e, error_code);
        return;
    }

    switch(gatt_client.proc) {
    case GattClientProcServices:
        FURI_LOG_I(TAG, "Services discovered: %u", gatt_client.num_services);
        if(gatt_client.num_services == 0) {
            gatt_client_disc_finish(e);
            return;
        }
        gatt_client.proc = GattClientProcChars;
        gatt_client.disc_service_idx = 0;
        gatt_client_disc_start_chars_for_current_service(e);
        break;

    case GattClientProcChars:
        gatt_client.disc_service_idx++;
        if(gatt_client.disc_service_idx < gatt_client.num_services) {
            gatt_client_disc_start_chars_for_current_service(e);
        } else {
            FURI_LOG_I(TAG, "Characteristics discovered: %u", gatt_client.num_chars);
            gatt_client.proc = GattClientProcDesc;
            gatt_client.disc_char_idx = 0;
            if(!gatt_client_disc_issue_next_desc_cmd()) {
                gatt_client_disc_finish(e);
            }
        }
        break;

    case GattClientProcDesc:
        gatt_client.disc_char_idx++;
        if(!gatt_client_disc_issue_next_desc_cmd()) {
            gatt_client_disc_finish(e);
        }
        break;

    case GattClientProcCharByUuid:
        gatt_client_disc_finish(e);
        break;

    case GattClientProcWriteDesc:
        FURI_LOG_I(TAG, "CCCD write complete");
        gatt_client.proc = GattClientProcIdle;
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Event payload parsers (caller holds the mutex)                      */
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
        svc->start_handle = gatt_client_read_u16(&p[0]);
        svc->end_handle = gatt_client_read_u16(&p[2]);
        if(record_len == GATT_CLIENT_SVC_REC_128) {
            memcpy(svc->uuid, p + 4, GATT_CLIENT_UUID128_LEN);
        } else {
            gatt_client_promote_uuid16(gatt_client_read_u16(&p[4]), svc->uuid);
        }
        p += record_len;
    }
}

static void gatt_client_add_char(
    uint8_t service_idx,
    uint16_t decl_handle,
    uint8_t properties,
    uint16_t value_handle,
    const uint8_t uuid_wire[GATT_CLIENT_UUID128_LEN]) {
    if(gatt_client_char_idx_by_value_handle(value_handle) >= 0) {
        return; /* already known */
    }
    if(gatt_client.num_chars >= GATT_CLIENT_MAX_CHARS) {
        FURI_LOG_W(TAG, "Char cap reached (%u), dropping rest", GATT_CLIENT_MAX_CHARS);
        return;
    }
    GattClientChar* ch = &gatt_client.chars[gatt_client.num_chars++];
    ch->service_idx = service_idx;
    ch->char_handle = decl_handle;
    ch->properties = properties;
    ch->value_handle = value_handle;
    memcpy(ch->uuid, uuid_wire, GATT_CLIENT_UUID128_LEN);
    ch->cccd_handle = 0;
    FURI_LOG_D(TAG, "char[%u] val=0x%04X props=0x%02X", gatt_client.num_chars - 1, value_handle, properties);
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
        uint8_t uuid[GATT_CLIENT_UUID128_LEN];
        if(record_len == GATT_CLIENT_CHAR_REC_128) {
            memcpy(uuid, p + 5, GATT_CLIENT_UUID128_LEN);
        } else {
            gatt_client_promote_uuid16(gatt_client_read_u16(&p[5]), uuid);
        }
        gatt_client_add_char(
            gatt_client.disc_service_idx,
            gatt_client_read_u16(&p[0]),
            p[2],
            gatt_client_read_u16(&p[3]),
            uuid);
        p += record_len;
    }
}

/* One "Discover Characteristics By UUID" result: the characteristic declaration
 * at Attribute_Handle, value = properties (1) + value handle (2) + UUID. */
static void gatt_client_parse_char_by_uuid(const aci_gatt_disc_read_char_by_uuid_resp_event_rp0* ev) {
    if(ev->Attribute_Value_Length != GATT_CLIENT_CHAR_DECL_128) {
        FURI_LOG_W(TAG, "Unexpected char declaration len %u", ev->Attribute_Value_Length);
        return;
    }
    const uint8_t* v = ev->Attribute_Value;
    if(memcmp(&v[3], gatt_client.target_uuid, GATT_CLIENT_UUID128_LEN) != 0) {
        return;
    }
    /* No service range is known for this char; index 0 is harmless because
     * subscribe() needs the CCCD of a full discovery anyway. */
    gatt_client_add_char(0, ev->Attribute_Handle, v[0], gatt_client_read_u16(&v[1]), &v[3]);
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
        if(format == 0x01 && gatt_client_read_u16(&p[2]) == GATT_CLIENT_CCCD_UUID16 &&
           ch->cccd_handle == 0) {
            ch->cccd_handle = gatt_client_read_u16(&p[0]);
        }
        /* 128-bit descriptors are not tracked (no CCCD is 128-bit). */
        p += pair_len;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void gatt_client_init(void) {
    furi_check(!gatt_client.initialized);
    memset(&gatt_client, 0, sizeof(gatt_client));
    gatt_client.mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    gatt_client.proc = GattClientProcIdle;
    gatt_client.att_mtu = BLE_DEFAULT_ATT_MTU;
    gatt_client.initialized = true;
    FURI_LOG_I(TAG, "init");
}

void gatt_client_deinit(void) {
    if(!gatt_client.initialized) {
        return;
    }
    furi_mutex_free(gatt_client.mutex);
    memset(&gatt_client, 0, sizeof(gatt_client));
    FURI_LOG_I(TAG, "deinit");
}

void gatt_client_reset(void) {
    if(!gatt_client.initialized) {
        return;
    }
    gatt_client_lock();
    gatt_client.attached = false;
    gatt_client.conn_handle = 0;
    gatt_client.proc = GattClientProcIdle;
    gatt_client.stale_procs = 0;
    gatt_client.att_mtu = BLE_DEFAULT_ATT_MTU;
    gatt_client_disc_reset_storage();
    gatt_client_unlock();
    FURI_LOG_I(TAG, "reset");
}

void gatt_client_attach(uint16_t conn_handle, uint32_t link_seq) {
    furi_check(gatt_client.initialized);
    gatt_client_lock();
    gatt_client.conn_handle = conn_handle;
    gatt_client.link_seq = link_seq;
    gatt_client.attached = true;
    gatt_client.proc = GattClientProcIdle;
    gatt_client.stale_procs = 0;
    gatt_client.att_mtu = BLE_DEFAULT_ATT_MTU;
    gatt_client_disc_reset_storage();
    gatt_client_unlock();
    FURI_LOG_I(TAG, "attach conn_handle=0x%04X", conn_handle);
}

bool gatt_client_discover(void) {
    furi_check(gatt_client.initialized);
    bool ok = false;
    gatt_client_lock();
    if(gatt_client_can_start("discover")) {
        tBleStatus status = aci_gatt_disc_all_primary_services(gatt_client.conn_handle);
        if(status == BLE_STATUS_SUCCESS) {
            /* Cleared only once the procedure runs, so a rejected call keeps the
             * previous table. The mutex is still held: no response is parsed
             * before this. */
            gatt_client_disc_reset_storage();
            gatt_client_start_proc(GattClientProcServices, GATT_CLIENT_DISCOVER_TIMEOUT_MS);
            ok = true;
        } else {
            FURI_LOG_E(TAG, "disc_all_primary_services failed: 0x%02X", status);
        }
    }
    gatt_client_unlock();
    return ok;
}

bool gatt_client_discover_char(const uint8_t uuid128_human[16]) {
    furi_check(gatt_client.initialized);
    furi_check(uuid128_human);
    bool ok = false;
    gatt_client_lock();
    if(gatt_client_can_start("discover_char")) {
        UUID_t uuid;
        gatt_client_reverse_uuid(uuid128_human, uuid.UUID_128);
        tBleStatus status = aci_gatt_disc_char_by_uuid(
            gatt_client.conn_handle, 0x0001, 0xFFFF, UUID_TYPE_128, &uuid);
        if(status == BLE_STATUS_SUCCESS) {
            /* The table is cleared so the result always finds a slot (a full
             * table would drop it and report the characteristic as absent).
             * Entries of an earlier discovery, full or targeted, are forgotten:
             * afterwards find_char() resolves only this UUID, and subscribe()
             * needs a new full discovery. Cleared only once the procedure runs,
             * so a rejected call keeps the previous table. The mutex is still
             * held: no response is parsed before the reset and the target. */
            gatt_client_disc_reset_storage();
            memcpy(gatt_client.target_uuid, uuid.UUID_128, GATT_CLIENT_UUID128_LEN);
            gatt_client_start_proc(GattClientProcCharByUuid, GATT_CLIENT_DISCOVER_CHAR_TIMEOUT_MS);
            ok = true;
        } else {
            FURI_LOG_E(TAG, "disc_char_by_uuid failed: 0x%02X", status);
        }
    }
    gatt_client_unlock();
    return ok;
}

bool gatt_client_write_nr(uint16_t char_value_handle, const uint8_t* data, size_t len) {
    furi_check(gatt_client.initialized);
    if(len > 0 && !data) {
        return false;
    }
    bool ok = false;
    gatt_client_lock();
    do {
        if(!gatt_client.attached) {
            FURI_LOG_E(TAG, "write_nr: not attached");
            break;
        }
        size_t max_len = gatt_client.att_mtu - GATT_CLIENT_ATT_HEADER;
        if(max_len > GATT_CLIENT_WRITE_CMD_MAX) max_len = GATT_CLIENT_WRITE_CMD_MAX;
        if(len > max_len) {
            FURI_LOG_E(TAG, "write_nr: len %u exceeds %u", (unsigned)len, (unsigned)max_len);
            break;
        }
        tBleStatus status = aci_gatt_write_without_resp(
            gatt_client.conn_handle, char_value_handle, (uint8_t)len, data);
        if(status != BLE_STATUS_SUCCESS) {
            FURI_LOG_E(
                TAG, "write_without_resp handle=0x%04X failed: 0x%02X", char_value_handle, status);
            break;
        }
        /* No completion event: write without response is not acknowledged. */
        ok = true;
    } while(false);
    gatt_client_unlock();
    return ok;
}

bool gatt_client_subscribe(uint16_t char_value_handle) {
    furi_check(gatt_client.initialized);
    bool ok = false;
    gatt_client_lock();
    do {
        if(!gatt_client_can_start("subscribe")) break;
        int idx = gatt_client_char_idx_by_value_handle(char_value_handle);
        if(idx < 0) {
            FURI_LOG_E(TAG, "subscribe: unknown handle 0x%04X", char_value_handle);
            break;
        }
        uint16_t cccd_handle = gatt_client.chars[idx].cccd_handle;
        if(cccd_handle == 0) {
            FURI_LOG_E(TAG, "subscribe: no CCCD for handle 0x%04X", char_value_handle);
            break;
        }
        const uint8_t cccd_value[2] = {0x01, 0x00};
        tBleStatus status = aci_gatt_write_char_desc(
            gatt_client.conn_handle, cccd_handle, sizeof(cccd_value), cccd_value);
        if(status != BLE_STATUS_SUCCESS) {
            FURI_LOG_E(TAG, "write_char_desc cccd=0x%04X failed: 0x%02X", cccd_handle, status);
            break;
        }
        gatt_client_start_proc(GattClientProcWriteDesc, GATT_CLIENT_WRITE_DESC_TIMEOUT_MS);
        FURI_LOG_I(TAG, "subscribe: handle=0x%04X cccd=0x%04X", char_value_handle, cccd_handle);
        ok = true;
    } while(false);
    gatt_client_unlock();
    return ok;
}

uint16_t gatt_client_find_char(const uint8_t uuid128_human[16]) {
    furi_check(gatt_client.initialized);
    furi_check(uuid128_human);

    uint8_t needle_wire[GATT_CLIENT_UUID128_LEN];
    gatt_client_reverse_uuid(uuid128_human, needle_wire);

    uint16_t handle = 0;
    gatt_client_lock();
    for(uint8_t i = 0; i < gatt_client.num_chars; i++) {
        if(memcmp(gatt_client.chars[i].uuid, needle_wire, GATT_CLIENT_UUID128_LEN) == 0) {
            handle = gatt_client.chars[i].value_handle;
            break;
        }
    }
    gatt_client_unlock();
    return handle;
}

void gatt_client_tick(void) {
    if(!gatt_client.initialized) {
        return;
    }
    GattClientEmit emit = {0};
    gatt_client_lock();
    if(gatt_client.attached && gatt_client.proc != GattClientProcIdle &&
       (int32_t)(furi_get_tick() - gatt_client.deadline) >= 0) {
        FURI_LOG_E(TAG, "Procedure %u timed out", gatt_client.proc);
        if(gatt_client.stale_procs < GATT_CLIENT_STALE_MAX) gatt_client.stale_procs++;
        gatt_client_proc_fail(&emit, 0xFF);
    }
    gatt_client_unlock();
    gatt_client_deliver(&emit);
}

/* ------------------------------------------------------------------ */
/* Dispatcher entry (BLE event thread)                                 */
/* ------------------------------------------------------------------ */

BleEventAckStatus gatt_client_handle_event(void* pckt) {
    furi_check(pckt);
    if(!gatt_client.initialized) {
        return BleEventNotAck;
    }

    hci_uart_pckt* uart_pckt = (hci_uart_pckt*)pckt;
    if(uart_pckt->type != HCI_EVENT_PKT_TYPE) {
        return BleEventNotAck;
    }
    hci_event_pckt* event_pckt = (hci_event_pckt*)uart_pckt->data;
    if(event_pckt->evt != HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE) {
        return BleEventNotAck;
    }

    evt_blecore_aci* blue_evt = (evt_blecore_aci*)event_pckt->data;
    switch(blue_evt->ecode) {
    case ACI_ATT_READ_BY_GROUP_TYPE_RESP_VSEVT_CODE:
    case ACI_ATT_READ_BY_TYPE_RESP_VSEVT_CODE:
    case ACI_ATT_FIND_INFO_RESP_VSEVT_CODE:
    case ACI_GATT_DISC_READ_CHAR_BY_UUID_RESP_VSEVT_CODE:
    case ACI_ATT_EXCHANGE_MTU_RESP_VSEVT_CODE:
    case ACI_GATT_PROC_COMPLETE_VSEVT_CODE:
    case ACI_GATT_PROC_TIMEOUT_VSEVT_CODE:
    case ACI_GATT_ERROR_RESP_VSEVT_CODE:
    case ACI_GATT_NOTIFICATION_VSEVT_CODE:
        break;
    default:
        return BleEventNotAck;
    }

    /* All events above start with the 16-bit Connection_Handle. */
    const uint16_t handle = gatt_client_read_u16(blue_evt->data);
    GattClientEmit emit = {0};

    gatt_client_lock();
    if(!gatt_client.attached || handle != gatt_client.conn_handle) {
        gatt_client_unlock();
        return BleEventNotAck;
    }

    switch(blue_evt->ecode) {
    case ACI_ATT_READ_BY_GROUP_TYPE_RESP_VSEVT_CODE:
        if(gatt_client.proc == GattClientProcServices) {
            aci_att_read_by_group_type_resp_event_rp0* ev =
                (aci_att_read_by_group_type_resp_event_rp0*)blue_evt->data;
            gatt_client_parse_service_records(
                ev->Attribute_Data_List, ev->Data_Length, ev->Attribute_Data_Length);
        }
        break;

    case ACI_ATT_READ_BY_TYPE_RESP_VSEVT_CODE:
        if(gatt_client.proc == GattClientProcChars) {
            aci_att_read_by_type_resp_event_rp0* ev =
                (aci_att_read_by_type_resp_event_rp0*)blue_evt->data;
            gatt_client_parse_char_records(
                ev->Handle_Value_Pair_Data, ev->Data_Length, ev->Handle_Value_Pair_Length);
        }
        break;

    case ACI_ATT_FIND_INFO_RESP_VSEVT_CODE:
        if(gatt_client.proc == GattClientProcDesc) {
            gatt_client_parse_desc_records((aci_att_find_info_resp_event_rp0*)blue_evt->data);
        }
        break;

    case ACI_GATT_DISC_READ_CHAR_BY_UUID_RESP_VSEVT_CODE:
        if(gatt_client.proc == GattClientProcCharByUuid) {
            gatt_client_parse_char_by_uuid(
                (aci_gatt_disc_read_char_by_uuid_resp_event_rp0*)blue_evt->data);
        }
        break;

    case ACI_ATT_EXCHANGE_MTU_RESP_VSEVT_CODE: {
        aci_att_exchange_mtu_resp_event_rp0* ev =
            (aci_att_exchange_mtu_resp_event_rp0*)blue_evt->data;
        uint16_t mtu = ev->Server_RX_MTU;
        if(mtu > CFG_BLE_MAX_ATT_MTU) mtu = CFG_BLE_MAX_ATT_MTU;
        if(mtu < BLE_DEFAULT_ATT_MTU) mtu = BLE_DEFAULT_ATT_MTU;
        gatt_client.att_mtu = mtu;
        FURI_LOG_I(TAG, "ATT_MTU %u", mtu);
        break;
    }

    case ACI_GATT_PROC_COMPLETE_VSEVT_CODE: {
        aci_gatt_proc_complete_event_rp0* ev = (aci_gatt_proc_complete_event_rp0*)blue_evt->data;
        if(gatt_client.stale_procs > 0) {
            /* Completion of a procedure abandoned at its deadline. */
            gatt_client.stale_procs--;
            FURI_LOG_I(TAG, "Late procedure-complete consumed");
        } else if(gatt_client.proc != GattClientProcIdle) {
            gatt_client_on_proc_complete(&emit, ev->Error_Code);
        }
        break;
    }

    case ACI_GATT_PROC_TIMEOUT_VSEVT_CODE:
        /* ATT transaction timeout: no further procedure completes on this link. */
        if(gatt_client.proc != GattClientProcIdle) {
            gatt_client_proc_fail(&emit, 0xFE);
        }
        break;

    case ACI_GATT_ERROR_RESP_VSEVT_CODE: {
        aci_gatt_error_resp_event_rp0* ev = (aci_gatt_error_resp_event_rp0*)blue_evt->data;
        FURI_LOG_D(
            TAG, "ATT error opcode=0x%02X attr=0x%04X err=0x%02X", ev->Req_Opcode,
            ev->Attribute_Handle, ev->Error_Code);
        break;
    }

    case ACI_GATT_NOTIFICATION_VSEVT_CODE: {
        aci_gatt_notification_event_rp0* ev = (aci_gatt_notification_event_rp0*)blue_evt->data;
        gatt_client_prepare(
            &emit,
            FuriHalBtCentralEventNotification,
            ev->Attribute_Handle,
            ev->Attribute_Value,
            ev->Attribute_Value_Length);
        break;
    }

    default:
        break;
    }
    gatt_client_unlock();

    gatt_client_deliver(&emit);
    return BleEventAckFlowEnable;
}
