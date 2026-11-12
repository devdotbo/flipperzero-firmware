/**
 * @file govee_h6006_app.c
 * Entry point, inbox, remote signals, engine bookkeeping and the
 * teardown order.
 */
#include "govee_h6006_app.h"

#include <bt/bt_service/bt.h>
#include <furi_hal_bt.h>
#include <string.h>

/* Signal-callback barrier. Static so it stays valid while the loader thread
 * is inside the callback: the loader cannot unload the app during that call
 * (loader.c:793), but the app thread could free its own data. */
static atomic_uint s_signal_inflight;
static atomic_bool s_signal_closing;

/* ------------------------------------------------------------------------- */
/* Inbox                                                                      */
/* ------------------------------------------------------------------------- */

bool govee_app_post(GoveeH6006App* app, const GoveeMsg* msg) {
    if(furi_message_queue_put(app->inbox, msg, 0) == FuriStatusOk) return true;
    atomic_fetch_add(&app->inbox_overflow, 1u);
    return false;
}

void govee_app_request_exit(GoveeH6006App* app) {
    if(app->exiting) return;
    FURI_LOG_I(GOVEE_TAG, "exit requested");
    app->exiting = true;
    app->lightshow = false;
    view_dispatcher_stop(app->view_dispatcher);
}

static void handle_remote(GoveeH6006App* app, const GoveeRemoteCmd* cmd) {
    uint32_t v = cmd->value;
    FURI_LOG_D(GOVEE_TAG, "inbox signal %u arg %lu", (unsigned)cmd->signal, v);
    switch(cmd->signal) {
    case GoveeSignalStatus:
        govee_files_write_status(app, v);
        break;
    case GoveeSignalPower:
        if(v == 0 && app->lightshow) govee_ui_lightshow(app, false);
        app->desired.power = (v != 0);
        govee_link_set_desired(app);
        break;
    case GoveeSignalBrightness:
        app->desired.brightness = (uint8_t)v;
        govee_link_set_desired(app);
        break;
    case GoveeSignalKelvin:
        if(app->lightshow) govee_ui_lightshow(app, false);
        app->desired.mode = GoveeModeWhite;
        app->desired.kelvin = (uint16_t)v;
        govee_link_set_desired(app);
        break;
    case GoveeSignalRgb:
        if(app->lightshow) govee_ui_lightshow(app, false);
        app->desired.mode = GoveeModeColour;
        app->desired.rgb = v & 0xFFFFFFu;
        app->colour_index = 0xFF;
        govee_link_set_desired(app);
        break;
    case GoveeSignalScan:
        if(!govee_link_scan_active(app)) {
            if(app->scan_state == GoveeScanNone) app->scan_state = GoveeScanIdle;
            govee_link_request_scan(app, false);
        }
        break;
    case GoveeSignalApply:
        govee_link_apply_again(app);
        break;
    case GoveeSignalForget:
        if(v >= 1 && v <= app->registry->count) {
            govee_ui_forget(app, (uint8_t)(v - 1u));
        } else {
            FURI_LOG_W(GOVEE_TAG, "forget %lu: no such lamp", v);
        }
        break;
    case GoveeSignalTarget:
        if(v <= app->registry->count) {
            govee_ui_set_target(app, (uint8_t)v);
        } else {
            FURI_LOG_W(GOVEE_TAG, "target %lu: no such lamp", v);
        }
        break;
    case GoveeSignalLightshow:
        govee_ui_lightshow(app, v != 0);
        break;
    case GoveeSignalClearEvents:
        govee_engine_clear_events(app->engine);
        break;
    default:
        break;
    }
    app->ui_dirty = true;
    govee_ui_refresh(app);
}

static void dispatch(GoveeH6006App* app, const GoveeMsg* msg) {
    switch(msg->type) {
    case GoveeMsgLink:
        FURI_LOG_D(
            GOVEE_TAG, "inbox link gen %lu ev %u h %u", msg->link.gen, msg->link.ev, msg->link.handle);
        govee_link_handle_msg(app, msg);
        break;
    case GoveeMsgScanReport:
        FURI_LOG_D(GOVEE_TAG, "inbox scan report %s", msg->scan.name);
        govee_link_handle_msg(app, msg);
        break;
    case GoveeMsgScanEnd:
        FURI_LOG_D(GOVEE_TAG, "inbox scan end %lu", msg->scan_end.scan_gen);
        govee_link_handle_msg(app, msg);
        break;
    case GoveeMsgTimer:
        govee_link_handle_msg(app, msg);
        break;
    case GoveeMsgRemote:
        handle_remote(app, &msg->remote);
        break;
    case GoveeMsgExit:
        FURI_LOG_D(GOVEE_TAG, "inbox exit");
        atomic_store(&app->latch_exit, false);
        govee_app_request_exit(app);
        break;
    case GoveeMsgUi:
        FURI_LOG_D(GOVEE_TAG, "inbox ui %u %lu", msg->ui.action, msg->ui.arg);
        govee_ui_handle_msg(app, msg->ui.action, msg->ui.arg);
        break;
    default:
        break;
    }
}

static void drain_inbox(GoveeH6006App* app) {
    GoveeMsg msg;
    for(uint32_t i = 0; i < GOVEE_INBOX_DRAIN_MAX; i++) {
        if(furi_message_queue_get(app->inbox, &msg, 0) != FuriStatusOk) break;
        dispatch(app, &msg);
    }
}

static void check_latches(GoveeH6006App* app) {
    if(atomic_exchange(&app->latch_exit, false)) govee_app_request_exit(app);
    /* The scan callback posts every lamp report before it sets the scan-end
     * latch, so reports queued ahead of the end are handled before the latch
     * finishes the scan (inbox and tick path alike). */
    if(atomic_load(&app->latch_scan_end) != 0) drain_inbox(app);
    govee_link_check_latches(app);
}

/* App thread (view dispatcher event loop): drain the inbox, then the latches. */
static void inbox_callback(FuriEventLoopObject* object, void* context) {
    UNUSED(object);
    GoveeH6006App* app = context;
    drain_inbox(app);
    check_latches(app);
}

/* App thread: deadlines and the safety net for lost wakeups. */
static void tick_callback(void* context) {
    GoveeH6006App* app = context;
    check_latches(app);
    govee_link_scan_tick(app);
    govee_ui_tick(app);
    govee_files_status_publish(app);
    if(app->hal_check && !app->exiting) govee_app_engine_after(app);
    if(app->ui_dirty) govee_ui_refresh(app);
}

static bool navigation_callback(void* context) {
    GoveeH6006App* app = context;
    if(!govee_ui_back(app)) govee_app_request_exit(app);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Loader thread: remote commands. Validates and posts, nothing else.        */
/* ------------------------------------------------------------------------- */

static bool signal_callback(uint32_t signal, void* arg, void* context) {
    atomic_fetch_add(&s_signal_inflight, 1u);
    bool handled = false;
    if(!atomic_load(&s_signal_closing)) {
        GoveeH6006App* app = context;
        GoveeMsg msg;
        memset(&msg, 0, sizeof(msg));
        if(signal == FuriSignalExit) {
            atomic_store(&app->latch_exit, true);
            msg.type = GoveeMsgExit;
            govee_app_post(app, &msg); /* the latch covers a full inbox */
            handled = true;
        } else {
            GoveeRemoteCmd cmd;
            if(govee_remote_validate(signal, (uint32_t)(uintptr_t)arg, &cmd)) {
                msg.type = GoveeMsgRemote;
                msg.remote = cmd;
                handled = govee_app_post(app, &msg);
            }
        }
    } else if(signal == FuriSignalExit) {
        handled = true; /* already exiting */
    }
    atomic_fetch_sub(&s_signal_inflight, 1u);
    return handled;
}

/* ------------------------------------------------------------------------- */
/* Engine bookkeeping after every engine call (app thread)                   */
/* ------------------------------------------------------------------------- */

static void log_engine(GoveeH6006App* app) {
    const GoveeEngine* e = app->engine;
    GoveeEngineState st = govee_engine_state(e);
    if(st != app->log_state) {
        FURI_LOG_D(
            GOVEE_TAG,
            "engine %s -> %s",
            govee_engine_state_name((GoveeEngineState)app->log_state),
            govee_engine_state_name(st));
        app->log_state = (uint8_t)st;
    }
    if(e->pass_no != app->log_pass) {
        app->log_pass = e->pass_no;
        app->log_in_pass = true;
        FURI_LOG_I(
            GOVEE_TAG,
            "pass %lu start, rev %lu, %u lamps targeted",
            e->pass_no,
            govee_engine_revision(e),
            govee_engine_targeted_count(e));
    }
    if(app->log_in_pass && !e->in_pass) {
        app->log_in_pass = false;
        FURI_LOG_I(
            GOVEE_TAG, "pass %lu end, %lu ms", e->pass_no, furi_get_tick() - e->pass_start_ms);
        for(uint8_t i = 0; i < e->lamp_count; i++) {
            const GoveeEngineLamp* l = govee_engine_lamp(e, i);
            if(!l || !l->targeted) continue;
            FURI_LOG_I(
                GOVEE_TAG,
                "  lamp %u %s rev %lu conn %lu disc %lu write %lu dis %lu total %lu ms%s",
                i + 1u,
                govee_engine_outcome_name((GoveeOutcome)l->outcome),
                l->reached_rev,
                l->times.connect_ms,
                l->times.discover_ms,
                l->times.write_ms,
                l->times.disconnect_ms,
                l->times.total_ms,
                l->times.reused ? " reused" : "");
        }
    }
}

/* Outside a pass, copy each lamp's result of its last completed attempt
 * into the registry and save the lamp file only if one of them changed.
 * A result whose save failed stays pending (results_unsaved, cleared by any
 * successful govee_files_save) and is saved again once per later pass, so a
 * failing SD card is not written on every engine call. A blocked save is not
 * retried; it only fails again when a result changes. */
static void save_lamp_results(GoveeH6006App* app) {
    const GoveeEngine* e = app->engine;
    if(e->in_pass) return;
    bool changed = false;
    for(uint8_t i = 0; i < app->registry->count; i++) {
        GoveeRegistryLamp* r = &app->registry->lamps[i];
        const GoveeEngineLamp* l = govee_engine_lamp(e, govee_engine_find_lamp(e, &r->id));
        if(!l || l->last == GoveeLampLastUnknown || l->last == r->last) continue;
        r->last = l->last;
        changed = true;
    }
    if(changed) app->results_unsaved = true;
    if(!app->results_unsaved) return;
    if(!changed && (app->save_blocked || e->pass_no == app->results_save_pass)) return;
    app->results_save_pass = e->pass_no;
    if(govee_files_save(app)) FURI_LOG_I(GOVEE_TAG, "lamp results saved");
}

void govee_app_engine_after(GoveeH6006App* app) {
    for(uint8_t i = 0; i < GOVEE_RECOVER_MAX; i++) {
        if(app->exiting || govee_engine_state(app->engine) != GoveeEngineFaulted) break;
        govee_link_recover(app);
    }
    if(!app->exiting) govee_link_hal_check(app);
    log_engine(app);
    if(app->exiting) return;
    save_lamp_results(app);
    govee_link_scan_kick(app);
    if(app->ui_dirty) govee_ui_refresh(app);
}

/* ------------------------------------------------------------------------- */
/* Teardown, app thread, every wait bounded                                  */
/* ------------------------------------------------------------------------- */

/* Deliver link events to the engine while it stops; everything else is dropped. */
static void teardown_pump(GoveeH6006App* app, uint32_t wait_ms) {
    GoveeMsg msg;
    if(furi_message_queue_get(app->inbox, &msg, furi_ms_to_ticks(wait_ms)) == FuriStatusOk) {
        if(msg.type == GoveeMsgLink) {
            govee_link_handle_msg(app, &msg);
        } else {
            FURI_LOG_D(GOVEE_TAG, "teardown: dropped inbox type %u", msg.type);
        }
    }
    govee_link_check_latches(app); /* terminal latches and the polled engine deadline */
}

static void teardown(GoveeH6006App* app, FuriEventLoop* loop) {
    GoveeEngine* e = app->engine;
    app->exiting = true;
    app->lightshow = false;
    FURI_LOG_I(GOVEE_TAG, "teardown, engine %s", govee_engine_state_name(govee_engine_state(e)));

    /* 1. The inbox leaves the event loop (polled below); stop the producers
     *    that can be stopped: the engine timer (free flushes the timer
     *    service, no timer callback runs after it) and a running scan. */
    furi_event_loop_unsubscribe(loop, app->inbox);
    app->timers_closed = true; /* engine deadlines are polled from here on */
    furi_timer_stop(app->engine_timer);
    furi_timer_free(app->engine_timer);
    app->engine_timer = NULL;
    if(app->scan_state == GoveeScanRunning || app->scan_state == GoveeScanStopping) {
        furi_hal_bt_central_stop_scan();
    }
    app->scan_state = GoveeScanIdle;

    /* 2. Stop the engine and keep delivering events until it is stopped or
     *    GOVEE_ENGINE_STOP_MAX_MS (+ margin) passed. */
    govee_engine_stop(e);
    govee_app_engine_after(app);
    uint32_t start = furi_get_tick();
    while(!govee_engine_is_stopped(e) && govee_engine_state(e) != GoveeEngineFaulted &&
          furi_get_tick() - start < GOVEE_TEARDOWN_ENGINE_MS) {
        teardown_pump(app, 10);
    }
    FURI_LOG_I(
        GOVEE_TAG,
        "engine %s after %lu ms",
        govee_engine_state_name(govee_engine_state(e)),
        furi_get_tick() - start);

    /* 3. Close a link the engine did not close, and wait (bounded). */
    if(app->link_open && app->conn) {
        FURI_LOG_W(GOVEE_TAG, "teardown: closing a leftover link");
        furi_hal_bt_central_disconnect(app->conn);
        start = furi_get_tick();
        while(app->link_open && furi_get_tick() - start < GOVEE_TEARDOWN_LINK_MS) {
            teardown_pump(app, 10);
        }
    }

    /* 4. Central deinit (bounded by the HAL; no HAL callback after it). */
    furi_hal_bt_central_deinit();
    app->conn = NULL;
    app->link_open = false;

    /* 5. Clear the thread signal callback, then wait for a call in progress. */
    atomic_store(&s_signal_closing, true);
    furi_thread_set_signal_callback(app->thread, NULL, NULL);
    start = furi_get_tick();
    while(atomic_load(&s_signal_inflight) != 0 &&
          furi_get_tick() - start < GOVEE_TEARDOWN_SIGNAL_MS) {
        furi_delay_ms(1);
    }
    /* A callback still counted in flight may use the inbox and the app
     * struct (its context): leak both rather than free memory it may touch. */
    bool signal_busy = atomic_load(&s_signal_inflight) != 0;

    /* 6. Drain and free the inbox (no producer is left). */
    if(signal_busy) {
        FURI_LOG_E(
            GOVEE_TAG,
            "signal callback still running after %u ms: inbox and app context leaked",
            (unsigned)GOVEE_TEARDOWN_SIGNAL_MS);
    } else {
        GoveeMsg msg;
        uint32_t dropped = 0;
        while(dropped < 2u * GOVEE_INBOX_DEPTH &&
              furi_message_queue_get(app->inbox, &msg, 0) == FuriStatusOk) {
            dropped++;
        }
        furi_message_queue_free(app->inbox);
        app->inbox = NULL;
        FURI_LOG_I(
            GOVEE_TAG,
            "inbox freed (%lu dropped, %u overflows)",
            dropped,
            atomic_load(&app->inbox_overflow));
    }

    /* 7. Restore the firmware's BT profile (peripheral BLE). */
    Bt* bt = furi_record_open(RECORD_BT);
    bt_profile_restore_default(bt);
    furi_record_close(RECORD_BT);

    /* 8. Free the rest. */
    govee_ui_free(app);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_GUI);
    free(app->line);
    free(app->text);
    free(app->engine);
    free(app->scratch);
    free(app->registry);
    if(!signal_busy) free(app);
}

/* ------------------------------------------------------------------------- */
/* Entry point                                                                */
/* ------------------------------------------------------------------------- */

int32_t govee_h6006_app(void* p) {
    UNUSED(p);

    /* Engine, registry and buffers on the heap, not on the 4 KB stack. */
    GoveeH6006App* app = malloc(sizeof(GoveeH6006App));
    memset(app, 0, sizeof(GoveeH6006App));
    app->registry = malloc(sizeof(GoveeRegistry));
    app->scratch = malloc(sizeof(GoveeRegistry));
    app->engine = malloc(sizeof(GoveeEngine));
    app->text = malloc(GOVEE_REGISTRY_TEXT_MAX);
    app->line = malloc(GOVEE_STATUS_LINE_MAX);
    atomic_init(&app->inbox_overflow, 0u);
    atomic_init(&app->cur_link_gen, 0u);
    atomic_init(&app->cur_scan_gen, 0u);
    atomic_init(&app->latch_term, 0u);
    atomic_init(&app->latch_scan_end, 0u);
    atomic_init(&app->latch_timer, false);
    atomic_init(&app->latch_exit, false);
    app->thread = furi_thread_get_current();
    /* One marker per run so the Mac tool can tell this run's status apart
     * from a file an earlier run left behind */
    app->session = furi_hal_random_get();
    if(app->session == 0u) app->session = furi_get_tick() | 1u;

    app->desired.power = true;
    app->desired.brightness = GOVEE_BRIGHTNESS_MAX;
    app->desired.mode = GoveeModeWhite;
    app->desired.kelvin = GOVEE_KELVIN_MIN;
    app->desired.rgb = 0xFF0000u;

    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    FURI_LOG_I(GOVEE_TAG, "start");

    govee_files_load(app); /* notices raised here are shown when Home opens */

    /* Suspend the firmware's peripheral BLE so the central scan is accepted. */
    Bt* bt = furi_record_open(RECORD_BT);
    bt_disconnect(bt);
    furi_delay_ms(200);
    furi_hal_bt_stop_advertising();
    furi_record_close(RECORD_BT);
    furi_hal_bt_central_init();

    app->inbox = furi_message_queue_alloc(GOVEE_INBOX_DEPTH, sizeof(GoveeMsg));
    app->engine_timer = furi_timer_alloc(govee_link_timer_callback, FuriTimerTypeOnce, app);
    govee_engine_init(app->engine, &govee_link_ops, app, &app->desired);
    app->log_state = GoveeEngineIdle;

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, navigation_callback);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, tick_callback, furi_ms_to_ticks(GOVEE_TICK_MS));
    govee_ui_alloc(app);

    FuriEventLoop* loop = view_dispatcher_get_event_loop(app->view_dispatcher);
    furi_event_loop_subscribe_message_queue(
        loop, app->inbox, FuriEventLoopEventIn, inbox_callback, app);

    /* Our callback replaces the event loop's default one, so FuriSignalExit
     * goes through the inbox and the app thread stops the loop itself. */
    atomic_store(&s_signal_inflight, 0u);
    atomic_store(&s_signal_closing, false);
    furi_thread_set_signal_callback(app->thread, signal_callback, app);

    /* Lamps, target and desired state to the engine: revision stays 0, nothing is sent. */
    govee_link_sync_engine(app);

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    if(app->registry->count > 0) {
        govee_ui_show(app, GoveeScreenHome);
    } else {
        govee_link_request_scan(app, true);
    }

    view_dispatcher_run(app->view_dispatcher);

    teardown(app, loop);
    return 0;
}
