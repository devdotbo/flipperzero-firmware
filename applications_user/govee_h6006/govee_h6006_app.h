/**
 * @file govee_h6006_app.h
 * App shell around the hardware-independent core in core/.
 *
 * Thread model:
 * - App thread: runs the view dispatcher's event loop. It alone touches the
 *   registry, the engine, the link and scan state, the UI and the SD card.
 * - BLE event thread and the HAL deadline thread: HAL scan and connection
 *   callbacks (govee_link.c). They read two atomics (current link and scan
 *   generation), latch terminal events, and post to the inbox with timeout 0.
 * - Timer service thread: the engine FuriTimer callback latches a flag and
 *   posts with timeout 0.
 * - Loader thread: the thread signal callback validates and posts with
 *   timeout 0; the exit request is latched too.
 * A failed post increments inbox_overflow. Latched items (link terminal
 *   events, timer fire, scan end, exit) are checked by the app thread after
 *   every drain and on every UI tick, so an overflow cannot lose them.
 */
#pragma once

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_bt_central.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/submenu.h>
#include <gui/modules/widget.h>
#include <gui/modules/dialog_ex.h>
#include <storage/storage.h>
#include <stdatomic.h>

#include "core/govee_types.h"
#include "core/govee_engine.h"
#include "core/govee_registry.h"
#include "core/govee_status.h"

#define GOVEE_TAG "Govee"

#define GOVEE_APP_DIR        "/ext/apps_data/govee_h6006"
#define GOVEE_APP_BULBS      GOVEE_APP_DIR "/bulbs.json"
#define GOVEE_APP_BULBS_NEW  GOVEE_APP_BULBS ".new"
#define GOVEE_APP_BULBS_BAK  GOVEE_APP_BULBS ".bak"
#define GOVEE_APP_BULBS_BAD  GOVEE_APP_BULBS ".corrupt"
#define GOVEE_APP_STATUS_TMP GOVEE_STATUS_PATH ".tmp"

/** Inbox depth. */
#define GOVEE_INBOX_DEPTH (32u)
/** Messages handled per inbox callback before the latches are checked. */
#define GOVEE_INBOX_DRAIN_MAX (64u)
/** UI tick period: scan deadlines, lightshow steps, latch and timer safety net. */
#define GOVEE_TICK_MS (100u)

/** Scan: quiet time after the last new lamp, and the hard cap. */
#define GOVEE_SCAN_QUIET_MS (1500u)
#define GOVEE_SCAN_CAP_MS   (8000u)
/** Wait for the HAL's scan-end report after stop_scan before giving up on it. */
#define GOVEE_SCAN_STOP_BACKSTOP_MS (1500u)

/** Lightshow step period (one lamp, kept link). */
#define GOVEE_LIGHTSHOW_STEP_MS (600u)

/** Teardown bounds. */
#define GOVEE_TEARDOWN_ENGINE_MS (GOVEE_ENGINE_STOP_MAX_MS + 500u)
#define GOVEE_TEARDOWN_LINK_MS   (1200u)
#define GOVEE_TEARDOWN_SIGNAL_MS (200u)

/** Ticks on which a status file that could not be moved into place is moved again. */
#define GOVEE_STATUS_PUBLISH_RETRIES (5u)

/** Fault recoveries in a row before the app stops trying until the next input. */
#define GOVEE_RECOVER_MAX (3u)

/* --- Inbox messages ------------------------------------------------------- */

typedef enum {
    GoveeMsgScanReport = 1,
    GoveeMsgScanEnd,
    GoveeMsgLink,
    GoveeMsgTimer,
    GoveeMsgRemote,
    GoveeMsgExit,
    GoveeMsgUi,
} GoveeMsgType;

typedef enum {
    GoveeLinkConnected = 0,
    GoveeLinkDiscovery,
    GoveeLinkDisconnected,
    GoveeLinkErrorLink,
    GoveeLinkErrorGatt,
} GoveeLinkEv;

/** Deferred UI actions (views must not be rebuilt inside their own callbacks). */
typedef enum {
    GoveeUiRetryScan = 0,
    GoveeUiLampsSelect, /**< arg = submenu index */
    GoveeUiLampDialog, /**< arg = DialogExResult */
    GoveeUiNoticeDone,
} GoveeUiAction;

typedef struct {
    uint8_t type; /**< GoveeMsgType */
    union {
        struct {
            uint32_t scan_gen;
            GoveeLampId id;
            int8_t rssi;
            uint8_t report_type;
            char name[GOVEE_NAME_LEN];
        } scan;
        struct {
            uint32_t scan_gen;
        } scan_end;
        struct {
            uint32_t gen;
            uint16_t handle;
            uint8_t ev; /**< GoveeLinkEv */
        } link;
        GoveeRemoteCmd remote;
        struct {
            uint8_t action; /**< GoveeUiAction */
            uint32_t arg;
        } ui;
    };
} GoveeMsg;

/* --- Screens and views ---------------------------------------------------- */

typedef enum {
    GoveeScreenNone = 0,
    GoveeScreenScan,
    GoveeScreenHome,
    GoveeScreenLamps,
    GoveeScreenLamp,
    GoveeScreenColour,
    GoveeScreenLightshow,
    GoveeScreenNotice,
} GoveeScreen;

typedef enum {
    GoveeViewHome = 0,
    GoveeViewLamps,
    GoveeViewWidget,
    GoveeViewDialog,
    GoveeViewRgbPicker,
} GoveeView;

typedef enum {
    GoveeScanIdle = 0, /**< no scan */
    GoveeScanWaiting, /**< requested; waits until the engine is idle and no link is open */
    GoveeScanRunning,
    GoveeScanStopping, /**< stop_scan issued, waiting for the HAL's end report */
    GoveeScanNone, /**< ended without any lamp: "No lamps found" screen */
} GoveeScanState;

/** Home list rows. */
typedef enum {
    GoveeHomePower = 0,
    GoveeHomeBrightness,
    GoveeHomeMode,
    GoveeHomeKelvin,
    GoveeHomeColour,
    GoveeHomeLightshow,
    GoveeHomeLamps,
    GoveeHomeFind,
    GoveeHomeResult,
} GoveeHomeRow;

typedef struct GoveeH6006App GoveeH6006App;

struct GoveeH6006App {
    /* --- written by producer threads (atomics only) --- */
    FuriMessageQueue* inbox;
    atomic_uint inbox_overflow;
    atomic_uint cur_link_gen; /**< link generation of the current connect call */
    atomic_uint cur_scan_gen; /**< generation of the current scan */
    atomic_uint latch_term; /**< (link gen << 2) | kind (1 Disconnected, 2 Error link), 0 none */
    atomic_uint latch_scan_end; /**< scan gen whose end report arrived, 0 none */
    atomic_bool latch_timer;
    atomic_bool latch_exit;

    /* --- app thread only below --- */
    FuriThread* thread;
    Gui* gui;
    Storage* storage;
    ViewDispatcher* view_dispatcher;
    VariableItemList* home;
    VariableItem* row[GoveeHomeResult + 1];
    Submenu* lamps;
    Widget* widget;
    DialogEx* dialog;
    View* rgb_view;

    GoveeRegistry* registry;
    GoveeRegistry* scratch; /**< parse target for file loads */
    GoveeEngine* engine;
    char* text; /**< GOVEE_REGISTRY_TEXT_MAX bytes for bulbs.json */
    char* line; /**< GOVEE_STATUS_LINE_MAX bytes for the status file */

    /* engine glue */
    FuriTimer* engine_timer;
    bool timers_closed; /**< teardown: the FuriTimer is gone, deadlines are polled */
    bool timer_armed;
    uint32_t timer_gen;
    uint32_t timer_deadline;
    FuriHalBtCentralConnection* conn;
    bool link_open; /**< a connect call is outstanding or a link is up (glue view) */
    uint32_t link_gen;
    uint16_t write_handle;
    uint32_t term_handled_gen; /**< last link gen whose terminal event was handled */
    uint32_t closed_gen; /**< link gen the engine was told is closed already (disconnect op false) */
    uint32_t recoveries;
    uint8_t recover_streak;
    bool hal_check; /**< check furi_hal_bt_central_available() in engine_after */

    /* scan */
    uint8_t scan_state; /**< GoveeScanState */
    bool scan_initial; /**< first-start scan (Back exits, nothing found offers retry) */
    bool scan_refused; /**< the central refused the last start_scan */
    uint32_t scan_gen;
    uint32_t scan_start_ms;
    uint32_t scan_last_new_ms;
    uint32_t scan_stop_ms;
    uint8_t scan_seen_count;
    uint8_t scan_new_count;
    GoveeLampId scan_seen[GOVEE_MAX_LAMPS];
    bool registry_full;
    bool engine_sync_pending; /**< lamp list or desired changed while a scan held the engine */
    bool apply_pending;

    /* desired state and target as the UI shows them */
    GoveeDesired desired;
    uint8_t target; /**< 0 all, n only lamp n */
    uint8_t colour_index; /**< picker palette index, 0xFF custom */

    /* lightshow */
    bool lightshow;
    uint8_t show_phase; /**< palette entry sent next */
    uint8_t show_sent; /**< palette entry sent last (label) */
    uint32_t show_next_ms;

    /* UI */
    GoveeScreen screen;
    GoveeScreen notice_return;
    uint8_t lamp_sel; /**< 0-based registry index on the Lamp screen */
    bool ui_dirty;
    bool exiting;
    char notice[64];
    bool notice_pending;
    char lamp_text[160];
    char widget_text[96];
    char value_text[24];

    /* files */
    const char* load_result;
    char save_result[40];
    uint32_t save_failures;
    bool save_blocked; /**< an invalid main file could not be preserved; never overwrite it */
    bool results_unsaved; /**< a lamp result is in the registry but not yet on the SD card */
    uint32_t results_save_pass; /**< engine pass of the last lamp result save attempt */
    uint32_t dump_seq;
    uint32_t session; /**< status file run marker: fixed for this run, never 0 */
    uint32_t dump_failures;
    uint8_t status_publish_retries; /**< moves of the finished status file left */

    /* logging */
    uint32_t log_pass;
    bool log_in_pass;
    uint8_t log_state;
};

/* --- govee_h6006_app.c ----------------------------------------------------- */
/** Post from any thread with timeout 0; counts an overflow. */
bool govee_app_post(GoveeH6006App* app, const GoveeMsg* msg);
/** App thread: run after every engine call (fault recovery, logs, UI refresh, pending scan). */
void govee_app_engine_after(GoveeH6006App* app);
/** App thread: request a clean exit (stops the view dispatcher). */
void govee_app_request_exit(GoveeH6006App* app);

/* --- govee_link.c (engine ops, HAL callbacks, scan) ------------------------ */
extern const GoveeEngineOps govee_link_ops;
void govee_link_timer_callback(void* context);
void govee_link_handle_msg(GoveeH6006App* app, const GoveeMsg* msg);
void govee_link_check_latches(GoveeH6006App* app);
void govee_link_check_timer(GoveeH6006App* app);
/** Push lamp list, target and desired state to the engine, or defer while a scan runs. */
void govee_link_sync_engine(GoveeH6006App* app);
void govee_link_set_desired(GoveeH6006App* app);
void govee_link_apply_again(GoveeH6006App* app);
/** Request a scan. initial: first-start scan. */
void govee_link_request_scan(GoveeH6006App* app, bool initial);
/** App thread: stop a running scan now (Back on the scan screen, teardown). */
void govee_link_abort_scan(GoveeH6006App* app);
void govee_link_scan_tick(GoveeH6006App* app);
void govee_link_scan_kick(GoveeH6006App* app);
bool govee_link_scan_active(const GoveeH6006App* app);
/** Faulted recovery: HAL deinit and init, then link_reset (engine contract). */
void govee_link_recover(GoveeH6006App* app);
/** If a HAL check is due (hal_check) and no link or scan holds the central, and
 *  furi_hal_bt_central_available() is false, run govee_link_recover(). Called from
 *  govee_app_engine_after() only. */
void govee_link_hal_check(GoveeH6006App* app);

/* --- govee_files.c ---------------------------------------------------------- */
void govee_files_load(GoveeH6006App* app);
/** Save the registry (.new, .bak, move). Failures are shown and counted. @return true on success */
bool govee_files_save(GoveeH6006App* app);
/** Write the status file for a status request (signal 100); req is its arg, echoed as req=. */
void govee_files_write_status(GoveeH6006App* app, uint32_t req);
/** App tick: move a finished status file into place if an earlier move failed. */
void govee_files_status_publish(GoveeH6006App* app);

/* --- govee_ui.c ------------------------------------------------------------- */
void govee_ui_alloc(GoveeH6006App* app);
void govee_ui_free(GoveeH6006App* app);
void govee_ui_show(GoveeH6006App* app, GoveeScreen screen);
void govee_ui_refresh(GoveeH6006App* app);
void govee_ui_notice(GoveeH6006App* app, const char* text);
void govee_ui_handle_msg(GoveeH6006App* app, uint32_t action, uint32_t arg);
bool govee_ui_back(GoveeH6006App* app);
void govee_ui_tick(GoveeH6006App* app);
void govee_ui_set_target(GoveeH6006App* app, uint8_t target);
void govee_ui_forget(GoveeH6006App* app, uint8_t index0);
bool govee_ui_lightshow(GoveeH6006App* app, bool on);
const char* govee_ui_state_token(const GoveeH6006App* app);

/* --- views/view_rgb_picker.c callbacks (app thread, input context) --------- */
void govee_app_on_colour(GoveeH6006App* app, uint8_t index, uint8_t r, uint8_t g, uint8_t b);
void govee_app_on_colour_back(GoveeH6006App* app);
