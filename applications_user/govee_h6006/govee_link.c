/**
 * @file govee_link.c
 * Engine glue (GoveeEngineOps on the BLE central HAL), HAL callbacks and the
 * scan flow.
 *
 * HAL callbacks (BLE event thread or the HAL deadline thread) and the timer
 * callback (timer service thread) only read atomics, latch, and post to the
 * inbox with timeout 0. Everything else in this file runs on the app thread.
 */
#include "govee_h6006_app.h"
#include "core/govee_h6006.h"

#include <string.h>

/* GOVEE_CHAR_WRITE_UUID "000102030405060708090a0b0c0d2b11" in the byte order
 * furi_hal_bt_central_find_char() and _gatt_discover_char() take. */
static const uint8_t govee_write_uuid[FURI_HAL_BT_CENTRAL_UUID128_LEN] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x2b, 0x11,
};

#define LATCH_DISCONNECTED (1u)
#define LATCH_ERROR_LINK   (2u)

/* ------------------------------------------------------------------------- */
/* Producer side: HAL and timer callbacks (not the app thread)               */
/* ------------------------------------------------------------------------- */

/* Copy the local name (AD type 0x08 or 0x09) out of advertising data. */
static bool adv_name(const uint8_t* data, uint8_t len, char* out, size_t out_size) {
    uint8_t i = 0;
    while(i < len && i < FURI_HAL_BT_CENTRAL_ADV_DATA_MAX) {
        uint8_t field = data[i];
        if(field == 0 || (uint16_t)i + field >= len) break;
        uint8_t type = data[i + 1u];
        if(type == 0x08u || type == 0x09u) {
            size_t n = (size_t)field - 1u;
            if(n >= out_size) n = out_size - 1u;
            memcpy(out, &data[i + 2u], n);
            out[n] = '\0';
            return true;
        }
        i = (uint8_t)(i + field + 1u);
    }
    return false;
}

/* BLE event thread: forward H6006 reports and the scan end. Never blocks. */
static void hal_scan_callback(const FuriHalBtCentralScanResult* result, void* context) {
    GoveeH6006App* app = context;
    if(!result) return;
    uint32_t gen = atomic_load(&app->cur_scan_gen);
    GoveeMsg msg;
    memset(&msg, 0, sizeof(msg));

    if(result->report_type == FURI_HAL_BT_CENTRAL_SCAN_REPORT_END) {
        atomic_store(&app->latch_scan_end, gen);
        msg.type = GoveeMsgScanEnd;
        msg.scan_end.scan_gen = gen;
        govee_app_post(app, &msg);
        return;
    }

    if(!adv_name(result->data, result->data_len, msg.scan.name, sizeof(msg.scan.name))) return;
    if(strstr(msg.scan.name, "H6006") == NULL) return;

    msg.type = GoveeMsgScanReport;
    msg.scan.scan_gen = gen;
    msg.scan.id.addr_type = result->addr_type;
    memcpy(msg.scan.id.addr, result->addr, GOVEE_ADDR_LEN);
    msg.scan.rssi = result->rssi;
    msg.scan.report_type = result->report_type;
    govee_app_post(app, &msg);
}

/* BLE event thread or HAL deadline thread: tag with the current link
 * generation (engine contract: one connect call at a time), latch terminal
 * events, post. find_char only reads the HAL's discovery result. */
static void hal_conn_callback(
    FuriHalBtCentralConnection* conn,
    FuriHalBtCentralEvent event,
    uint16_t char_handle,
    const uint8_t* data,
    size_t len,
    void* context) {
    UNUSED(data);
    UNUSED(len);
    GoveeH6006App* app = context;
    uint32_t gen = atomic_load(&app->cur_link_gen);
    GoveeMsg msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = GoveeMsgLink;
    msg.link.gen = gen;

    switch(event) {
    case FuriHalBtCentralEventConnected:
        msg.link.ev = GoveeLinkConnected;
        break;
    case FuriHalBtCentralEventDiscoveryComplete:
        msg.link.ev = GoveeLinkDiscovery;
        msg.link.handle = furi_hal_bt_central_find_char(conn, govee_write_uuid);
        break;
    case FuriHalBtCentralEventDisconnected:
        msg.link.ev = GoveeLinkDisconnected;
        atomic_store(&app->latch_term, (gen << 2) | LATCH_DISCONNECTED);
        break;
    case FuriHalBtCentralEventError:
        if(char_handle == FURI_HAL_BT_CENTRAL_ERROR_LINK) {
            msg.link.ev = GoveeLinkErrorLink;
            atomic_store(&app->latch_term, (gen << 2) | LATCH_ERROR_LINK);
        } else {
            msg.link.ev = GoveeLinkErrorGatt;
        }
        break;
    default:
        return; /* WriteComplete (not emitted), Notification: not used */
    }
    govee_app_post(app, &msg);
}

/* Timer service thread: wake the app thread. The deadline itself is checked
 * on the app thread, so a fire that raced a restart cannot fire early. */
void govee_link_timer_callback(void* context) {
    GoveeH6006App* app = context;
    atomic_store(&app->latch_timer, true);
    GoveeMsg msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = GoveeMsgTimer;
    govee_app_post(app, &msg);
}

/* ------------------------------------------------------------------------- */
/* Engine ops (app thread, called from inside the engine)                    */
/* ------------------------------------------------------------------------- */

static bool op_connect(void* ctx, const GoveeLampId* id, uint32_t link_gen) {
    GoveeH6006App* app = ctx;
    app->link_gen = link_gen;
    app->write_handle = 0;
    atomic_store(&app->cur_link_gen, link_gen); /* before connect: events may arrive at once */
    FuriHalBtCentralConnection* conn =
        furi_hal_bt_central_connect(id->addr, id->addr_type, hal_conn_callback, app);
    if(!conn) {
        app->conn = NULL;
        app->link_open = false;
        app->hal_check = true; /* checked in govee_app_engine_after, outside this op */
        return false;
    }
    app->conn = conn;
    app->link_open = true;
    return true;
}

static bool op_cancel_connect(void* ctx, uint32_t link_gen) {
    GoveeH6006App* app = ctx;
    if(!app->link_open || !app->conn || link_gen != app->link_gen) return false;
    furi_hal_bt_central_disconnect(app->conn); /* cancels while connecting, does not wait */
    return true;
}

static bool op_discover(void* ctx, uint32_t link_gen) {
    GoveeH6006App* app = ctx;
    if(!app->link_open || !app->conn || link_gen != app->link_gen) return false;
    app->write_handle = 0;
    return furi_hal_bt_central_gatt_discover_char(app->conn, govee_write_uuid);
}

static bool op_write(void* ctx, uint32_t link_gen, const uint8_t* data, size_t len) {
    GoveeH6006App* app = ctx;
    if(!app->link_open || !app->conn || link_gen != app->link_gen || app->write_handle == 0) {
        return false;
    }
    return furi_hal_bt_central_gatt_write_nr(app->conn, app->write_handle, data, len);
}

static bool op_disconnect(void* ctx, uint32_t link_gen) {
    GoveeH6006App* app = ctx;
    uint32_t latched = atomic_load(&app->latch_term);
    bool terminal_seen = latched != 0 && (latched >> 2) == (link_gen & 0x3FFFFFFFu);
    if(!app->link_open || !app->conn || link_gen != app->link_gen || terminal_seen) {
        /* nothing to disconnect: the engine treats the link as closed now */
        app->link_open = false;
        app->conn = NULL;
        app->write_handle = 0;
        app->closed_gen = link_gen;
        return false;
    }
    furi_hal_bt_central_disconnect(app->conn);
    return true;
}

static void op_timer_start(void* ctx, uint32_t ms, uint32_t timer_gen) {
    GoveeH6006App* app = ctx;
    if(ms == 0) ms = 1;
    app->timer_gen = timer_gen;
    app->timer_armed = true;
    app->timer_deadline = furi_get_tick() + ms;
    if(!app->timers_closed) furi_timer_start(app->engine_timer, furi_ms_to_ticks(ms));
}

static void op_timer_stop(void* ctx) {
    GoveeH6006App* app = ctx;
    app->timer_armed = false;
    if(!app->timers_closed) furi_timer_stop(app->engine_timer);
}

static uint32_t op_now_ms(void* ctx) {
    UNUSED(ctx);
    return furi_get_tick();
}

static void op_changed(void* ctx) {
    GoveeH6006App* app = ctx;
    app->ui_dirty = true;
}

const GoveeEngineOps govee_link_ops = {
    .connect = op_connect,
    .cancel_connect = op_cancel_connect,
    .discover = op_discover,
    .write = op_write,
    .disconnect = op_disconnect,
    .timer_start = op_timer_start,
    .timer_stop = op_timer_stop,
    .now_ms = op_now_ms,
    .changed = op_changed,
};

/* ------------------------------------------------------------------------- */
/* App thread: link events into the engine                                   */
/* ------------------------------------------------------------------------- */

static void handle_link(GoveeH6006App* app, uint32_t gen, uint8_t ev, uint16_t handle) {
    GoveeEngine* e = app->engine;
    bool terminal = (ev == GoveeLinkDisconnected || ev == GoveeLinkErrorLink);
    if(terminal) {
        if(gen == app->term_handled_gen) return; /* message and latch both arrived */
        app->term_handled_gen = gen;
        if(gen == app->link_gen) {
            app->link_open = false;
            app->conn = NULL;
            app->write_handle = 0;
        }
        if(gen == app->closed_gen) return; /* the engine already treated it as closed */
    }
    if(ev == GoveeLinkDiscovery && gen == app->link_gen) app->write_handle = handle;

    switch(ev) {
    case GoveeLinkConnected:
        govee_engine_on_connected(e, gen);
        break;
    case GoveeLinkDiscovery:
        govee_engine_on_discovery(e, gen, handle != 0);
        break;
    case GoveeLinkDisconnected:
        govee_engine_on_disconnected(e, gen);
        break;
    default: /* Error: link (terminal) or GATT (discovery failed, link stays up) */
        govee_engine_on_link_error(e, gen);
        break;
    }
    govee_app_engine_after(app);
}

void govee_link_check_timer(GoveeH6006App* app) {
    if(!app->timer_armed) return;
    if(!govee_time_reached(furi_get_tick(), app->timer_deadline)) return;
    app->timer_armed = false;
    govee_engine_on_timer(app->engine, app->timer_gen);
    govee_app_engine_after(app);
}

/* ------------------------------------------------------------------------- */
/* Engine sync (held while a scan runs: a connect would end the scan)        */
/* ------------------------------------------------------------------------- */

static bool scan_holds_engine(const GoveeH6006App* app) {
    return app->scan_state == GoveeScanRunning || app->scan_state == GoveeScanStopping;
}

bool govee_link_scan_active(const GoveeH6006App* app) {
    return app->scan_state == GoveeScanRunning || app->scan_state == GoveeScanStopping ||
           app->scan_state == GoveeScanWaiting;
}

void govee_link_sync_engine(GoveeH6006App* app) {
    if(scan_holds_engine(app)) {
        app->engine_sync_pending = true;
        return;
    }
    app->engine_sync_pending = false;
    GoveeLampId ids[GOVEE_MAX_LAMPS];
    uint8_t last[GOVEE_MAX_LAMPS]; /* saved result of each lamp's last completed attempt */
    uint8_t n = app->registry->count;
    for(uint8_t i = 0; i < n; i++) {
        ids[i] = app->registry->lamps[i].id;
        last[i] = app->registry->lamps[i].last;
    }
    if(app->target > n) app->target = 0;
    govee_engine_set_lamps_last(app->engine, ids, last, n, app->target);
    govee_engine_set_desired(app->engine, &app->desired);
    govee_app_engine_after(app);
}

/* A command while nothing was ever sent in this run (engine revision 0,
 * e.g. "on" equal to the start values) sends the full state. Opening the app
 * calls govee_link_sync_engine, not this, and still sends nothing. */
void govee_link_set_desired(GoveeH6006App* app) {
    if(scan_holds_engine(app)) {
        app->engine_sync_pending = true;
        if(app->engine->rev == 0) app->apply_pending = true;
        app->ui_dirty = true;
        govee_ui_refresh(app);
        return;
    }
    if(govee_engine_set_desired(app->engine, &app->desired) == 0) {
        govee_link_apply_again(app);
        return;
    }
    govee_app_engine_after(app);
}

void govee_link_apply_again(GoveeH6006App* app) {
    if(scan_holds_engine(app)) {
        app->apply_pending = true;
        return;
    }
    uint32_t rev = govee_engine_apply_again(app->engine);
    FURI_LOG_I(GOVEE_TAG, "apply again, rev %lu", rev);
    govee_app_engine_after(app);
}

/* ------------------------------------------------------------------------- */
/* Fault recovery (engine contract: HAL deinit and init, then link_reset)    */
/* ------------------------------------------------------------------------- */

void govee_link_recover(GoveeH6006App* app) {
    GoveeEngine* e = app->engine;
    app->recoveries++;
    FURI_LOG_W(
        GOVEE_TAG,
        "engine %s (fault %s), central %s, reinitialising the central (%lu)",
        govee_engine_state_name(govee_engine_state(e)),
        govee_engine_fault_name((GoveeEngineFault)e->fault),
        furi_hal_bt_central_available() ? "available" : "faulted",
        app->recoveries);
    furi_hal_bt_central_deinit(); /* bounded; barrier: no callback of the old link after this */
    furi_hal_bt_central_init();
    app->closed_gen = app->link_gen; /* a late terminal of the old link is not forwarded */
    app->link_open = false;
    app->conn = NULL;
    app->write_handle = 0;
    if(scan_holds_engine(app)) {
        /* deinit ended the scan; its end report may be lost with the handler */
        app->scan_state = GoveeScanNone;
        FURI_LOG_W(GOVEE_TAG, "scan ended by the central reset");
        app->ui_dirty = true;
    }
    govee_engine_link_reset(e); /* engine contract: ignored unless the engine is Faulted */
}

void govee_link_hal_check(GoveeH6006App* app) {
    if(!app->hal_check) return;
    /* only while no connect call, link or scan holds the central; else checked later */
    if(app->link_open || scan_holds_engine(app)) return;
    app->hal_check = false;
    if(furi_hal_bt_central_available()) return;
    FURI_LOG_W(GOVEE_TAG, "central faulted while the engine is not");
    govee_link_recover(app);
}

/* ------------------------------------------------------------------------- */
/* Scan                                                                      */
/* ------------------------------------------------------------------------- */

static void scan_start(GoveeH6006App* app) {
    app->scan_gen++;
    if(app->scan_gen == 0) app->scan_gen = 1;
    atomic_store(&app->cur_scan_gen, app->scan_gen);
    app->scan_seen_count = 0;
    app->scan_new_count = 0;
    app->scan_start_ms = furi_get_tick();
    app->scan_last_new_ms = app->scan_start_ms;
    if(!furi_hal_bt_central_start_scan(hal_scan_callback, app)) {
        FURI_LOG_E(GOVEE_TAG, "scan refused by the central");
        app->scan_state = GoveeScanNone;
        app->scan_refused = true;
        app->hal_check = true; /* the next engine_after (or tick) checks the central */
    } else {
        app->scan_refused = false;
        app->scan_state = GoveeScanRunning;
        FURI_LOG_I(GOVEE_TAG, "scan start (%lu)", app->scan_gen);
    }
    app->ui_dirty = true;
    govee_ui_refresh(app);
}

static void scan_finish(GoveeH6006App* app, const char* why) {
    uint32_t ms = furi_get_tick() - app->scan_start_ms;
    FURI_LOG_I(
        GOVEE_TAG,
        "scan end (%s): %u seen, %u new, %lu ms, %u saved",
        why,
        app->scan_seen_count,
        app->scan_new_count,
        ms,
        app->registry->count);
    if(app->scan_seen_count > 0) {
        app->scan_state = GoveeScanIdle;
        if(app->screen == GoveeScreenScan) govee_ui_show(app, GoveeScreenHome);
    } else {
        app->scan_state = GoveeScanNone;
    }
    /* A scan stop the controller did not confirm faults the central; recover
     * it before the deferred engine work connects again. */
    app->hal_check = true;
    govee_app_engine_after(app);
    if(app->engine_sync_pending) govee_link_sync_engine(app);
    if(app->apply_pending) {
        app->apply_pending = false;
        govee_link_apply_again(app);
    }
    app->ui_dirty = true;
    govee_ui_refresh(app);
}

static void scan_stop(GoveeH6006App* app, const char* why) {
    FURI_LOG_I(GOVEE_TAG, "scan stop (%s)", why);
    app->scan_stop_ms = furi_get_tick();
    app->scan_state = GoveeScanStopping;
    if(!furi_hal_bt_central_stop_scan()) scan_finish(app, "stop refused");
}

void govee_link_scan_kick(GoveeH6006App* app) {
    if(app->scan_state != GoveeScanWaiting || app->exiting) return;
    GoveeEngine* e = app->engine;
    if(govee_engine_is_busy(e) && govee_engine_state(e) == GoveeEngineKept) {
        FURI_LOG_I(GOVEE_TAG, "closing the kept link for a scan");
        govee_engine_release_link(e);
        app->ui_dirty = true;
    }
    if(govee_engine_is_busy(e) || app->link_open) return; /* engine_after kicks again */
    scan_start(app);
}

void govee_link_request_scan(GoveeH6006App* app, bool initial) {
    if(app->scan_state == GoveeScanRunning || app->scan_state == GoveeScanStopping ||
       app->scan_state == GoveeScanWaiting) {
        govee_ui_show(app, GoveeScreenScan);
        return;
    }
    if(app->lightshow) govee_ui_lightshow(app, false);
    app->scan_initial = initial;
    app->scan_state = GoveeScanWaiting;
    app->scan_refused = false; /* the status file reports the result of this request */
    govee_ui_show(app, GoveeScreenScan);
    govee_link_scan_kick(app);
}

void govee_link_abort_scan(GoveeH6006App* app) {
    switch(app->scan_state) {
    case GoveeScanWaiting:
        app->scan_state = GoveeScanIdle;
        break;
    case GoveeScanRunning:
        scan_stop(app, "back");
        break;
    default:
        break;
    }
}

void govee_link_scan_tick(GoveeH6006App* app) {
    uint32_t now = furi_get_tick();
    switch(app->scan_state) {
    case GoveeScanWaiting:
        govee_link_scan_kick(app);
        break;
    case GoveeScanRunning:
        if(now - app->scan_start_ms >= GOVEE_SCAN_CAP_MS) {
            scan_stop(app, "cap");
        } else if(app->scan_seen_count > 0 && now - app->scan_last_new_ms >= GOVEE_SCAN_QUIET_MS) {
            scan_stop(app, "quiet");
        }
        break;
    case GoveeScanStopping:
        if(now - app->scan_stop_ms >= GOVEE_SCAN_STOP_BACKSTOP_MS) {
            FURI_LOG_W(GOVEE_TAG, "no scan-end report after stop");
            scan_finish(app, "backstop");
        }
        break;
    default:
        break;
    }
}

static void handle_scan_end(GoveeH6006App* app, uint32_t gen) {
    if(gen != app->scan_gen) return;
    if(app->scan_state == GoveeScanRunning || app->scan_state == GoveeScanStopping) {
        scan_finish(app, app->scan_state == GoveeScanRunning ? "hal" : "stopped");
    }
}

static void handle_scan_report(GoveeH6006App* app, const GoveeMsg* msg) {
    if(msg->scan.scan_gen != app->scan_gen || !scan_holds_engine(app)) return;
    const GoveeLampId* id = &msg->scan.id;

    bool seen = false;
    for(uint8_t i = 0; i < app->scan_seen_count; i++) {
        if(govee_lamp_id_equal(&app->scan_seen[i], id)) {
            seen = true;
            break;
        }
    }
    if(!seen && app->scan_seen_count < GOVEE_MAX_LAMPS) {
        app->scan_seen[app->scan_seen_count++] = *id;
        app->scan_last_new_ms = furi_get_tick(); /* quiet timer restarts only for a new identity */
    }

    char old_name[GOVEE_NAME_LEN] = {0};
    uint8_t idx = govee_registry_find(app->registry, id);
    if(idx != GOVEE_LAMP_NONE) memcpy(old_name, app->registry->lamps[idx].name, sizeof(old_name));

    GoveeRegistryResult r = govee_registry_upsert(
        app->registry, msg->scan.name, id, furi_hal_rtc_get_timestamp(), &idx);
    char addr[18];
    govee_registry_format_addr(id->addr, addr);
    switch(r) {
    case GoveeRegistryAdded:
        app->scan_new_count++;
        FURI_LOG_I(
            GOVEE_TAG, "lamp found: %s %s rssi %d", app->registry->lamps[idx].name, addr, msg->scan.rssi);
        if(govee_files_save(app)) FURI_LOG_I(GOVEE_TAG, "lamp saved as %u", idx + 1u);
        govee_link_sync_engine(app); /* deferred until the scan ends */
        break;
    case GoveeRegistryUpdated:
        if(strcmp(old_name, app->registry->lamps[idx].name) != 0) {
            FURI_LOG_I(GOVEE_TAG, "lamp %u renamed: %s", idx + 1u, app->registry->lamps[idx].name);
            govee_files_save(app);
        }
        break;
    case GoveeRegistryFull:
        if(!app->registry_full) {
            app->registry_full = true;
            FURI_LOG_W(GOVEE_TAG, "registry full, %s not saved", addr);
            govee_ui_notice(app, "16 lamps saved. Remove one to add more.");
        }
        break;
    default:
        break;
    }
    app->ui_dirty = true;
}

/* ------------------------------------------------------------------------- */
/* App thread: inbox dispatch for link, scan and timer messages              */
/* ------------------------------------------------------------------------- */

void govee_link_handle_msg(GoveeH6006App* app, const GoveeMsg* msg) {
    switch(msg->type) {
    case GoveeMsgLink:
        handle_link(app, msg->link.gen, msg->link.ev, msg->link.handle);
        break;
    case GoveeMsgScanReport:
        handle_scan_report(app, msg);
        break;
    case GoveeMsgScanEnd:
        if(atomic_load(&app->latch_scan_end) == msg->scan_end.scan_gen) {
            atomic_store(&app->latch_scan_end, 0u);
        }
        handle_scan_end(app, msg->scan_end.scan_gen);
        break;
    case GoveeMsgTimer:
        atomic_store(&app->latch_timer, false);
        govee_link_check_timer(app);
        break;
    default:
        break;
    }
}

void govee_link_check_latches(GoveeH6006App* app) {
    uint32_t term = atomic_exchange(&app->latch_term, 0u);
    if(term != 0) {
        uint8_t ev = ((term & 3u) == LATCH_DISCONNECTED) ? GoveeLinkDisconnected :
                                                           GoveeLinkErrorLink;
        handle_link(app, term >> 2, ev, 0);
    }
    uint32_t scan_end = atomic_exchange(&app->latch_scan_end, 0u);
    if(scan_end != 0) handle_scan_end(app, scan_end);
    atomic_store(&app->latch_timer, false);
    govee_link_check_timer(app);
}
