#include "../govee_h6006_app.h"
#include "scenes.h"

#include <string.h>

#define TAG "GoveeSceneSaved"

#define SAVED_CONNECT_TIMEOUT_MS 5000

// Custom event IDs (scoped to this scene)
#define SAVED_EVENT_CONNECT_CLICK 0x4000 // | (idx & 0xFF)
#define SAVED_EVENT_TIMEOUT       0x4100
#define SAVED_EVENT_DIALOG_KEEP   0x4200
#define SAVED_EVENT_DIALOG_REMOVE 0x4201

typedef enum {
    SavedStateIdle,
    SavedStateConnecting,
    SavedStatePrompt,
} SavedState;

static SavedState saved_state = SavedStateIdle;
static uint32_t saved_selected_idx = 0;
static FuriTimer* saved_connect_timer = NULL;

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

static void saved_submenu_callback(void* context, uint32_t index) {
    GoveeH6006App* app = context;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, (uint32_t)(SAVED_EVENT_CONNECT_CLICK | (index & 0xFF)));
}

static void saved_timer_callback(void* context) {
    GoveeH6006App* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, SAVED_EVENT_TIMEOUT);
}

static void saved_dialog_callback(DialogExResult result, void* context) {
    GoveeH6006App* app = context;
    if(result == DialogExResultLeft) {
        view_dispatcher_send_custom_event(app->view_dispatcher, SAVED_EVENT_DIALOG_REMOVE);
    } else {
        // Right or Center = Keep
        view_dispatcher_send_custom_event(app->view_dispatcher, SAVED_EVENT_DIALOG_KEEP);
    }
}

// ---------------------------------------------------------------------------
// Submenu build
// ---------------------------------------------------------------------------

static void rebuild_submenu(GoveeH6006App* app) {
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Saved Bulbs");

    size_t count = govee_bulb_cache_count(app->cache);
    for(size_t i = 0; i < count; i++) {
        const GoveeBulbCacheEntry* e = govee_bulb_cache_get(app->cache, i);
        if(!e) continue;
        static char label[64];
        snprintf(
            label,
            sizeof(label),
            "%s %02X:%02X:%02X",
            e->name,
            e->addr[3],
            e->addr[4],
            e->addr[5]);
        submenu_add_item(app->submenu, label, (uint32_t)i, saved_submenu_callback, app);
    }

    if(count == 0) {
        submenu_add_item(app->submenu, "(no saved bulbs)", 0, NULL, NULL);
    }
}

// ---------------------------------------------------------------------------
// State transitions
// ---------------------------------------------------------------------------

static void cancel_connect_timer(void) {
    if(saved_connect_timer) {
        furi_timer_stop(saved_connect_timer);
    }
}

static void enter_state_idle(GoveeH6006App* app) {
    saved_state = SavedStateIdle;
    cancel_connect_timer();
    rebuild_submenu(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewSubmenu);
}

static void enter_state_connecting(GoveeH6006App* app, uint32_t idx) {
    const GoveeBulbCacheEntry* e = govee_bulb_cache_get(app->cache, idx);
    if(!e) {
        enter_state_idle(app);
        return;
    }

    saved_state = SavedStateConnecting;
    saved_selected_idx = idx;

    if(govee_central_is_connected(app->central)) {
        govee_central_disconnect(app->central);
    }

    popup_reset(app->popup);
    popup_set_header(app->popup, "Connecting", 64, 10, AlignCenter, AlignTop);
    static char msg[48];
    snprintf(msg, sizeof(msg), "%s\nPlease wait...", e->name);
    popup_set_text(app->popup, msg, 64, 32, AlignCenter, AlignCenter);
    popup_set_context(app->popup, app);
    popup_disable_timeout(app->popup);
    view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewPopup);

    app->selected_bulb = -1; // cache-origin; no scan_results entry
    memcpy(app->current_addr, e->addr, 6);
    app->current_addr_type = e->addr_type;
    app->has_current_addr = true;
    govee_central_connect(app->central, e->addr, e->addr_type);

    if(!saved_connect_timer) {
        saved_connect_timer = furi_timer_alloc(saved_timer_callback, FuriTimerTypeOnce, app);
    }
    furi_timer_start(saved_connect_timer, SAVED_CONNECT_TIMEOUT_MS);
}

static void enter_state_prompt(GoveeH6006App* app) {
    saved_state = SavedStatePrompt;
    cancel_connect_timer();

    // Ensure any dangling connection attempt is torn down
    if(govee_central_is_connected(app->central)) {
        govee_central_disconnect(app->central);
    }

    const GoveeBulbCacheEntry* e = govee_bulb_cache_get(app->cache, saved_selected_idx);
    const char* name = e ? e->name : "bulb";

    dialog_ex_reset(app->dialog_ex);
    dialog_ex_set_header(app->dialog_ex, "Not reachable", 64, 8, AlignCenter, AlignTop);
    static char body[64];
    snprintf(body, sizeof(body), "%s\ncan't be reached.", name);
    dialog_ex_set_text(app->dialog_ex, body, 64, 28, AlignCenter, AlignCenter);
    dialog_ex_set_left_button_text(app->dialog_ex, "Remove");
    dialog_ex_set_right_button_text(app->dialog_ex, "Keep");
    dialog_ex_set_context(app->dialog_ex, app);
    dialog_ex_set_result_callback(app->dialog_ex, saved_dialog_callback);

    view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewDialogEx);
}

// ---------------------------------------------------------------------------
// Scene handlers
// ---------------------------------------------------------------------------

void govee_scene_saved_on_enter(void* ctx) {
    GoveeH6006App* app = ctx;
    furi_assert(app);

    saved_state = SavedStateIdle;
    saved_selected_idx = 0;
    rebuild_submenu(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewSubmenu);
}

bool govee_scene_saved_on_event(void* ctx, SceneManagerEvent event) {
    GoveeH6006App* app = ctx;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        uint32_t ev = event.event;

        if((ev & 0xFF00) == (SAVED_EVENT_CONNECT_CLICK & 0xFF00) && (ev & 0xF000) == 0x4000) {
            uint32_t idx = ev & 0xFF;
            if(idx < govee_bulb_cache_count(app->cache)) {
                enter_state_connecting(app, idx);
            }
            consumed = true;

        } else if(saved_state == SavedStateConnecting && ev == GoveeCustomEventDiscoveryComplete) {
            cancel_connect_timer();
            saved_state = SavedStateIdle;
            scene_manager_next_scene(app->scene_manager, GoveeSceneControl);
            consumed = true;

        } else if(
            saved_state == SavedStateConnecting &&
            (ev == GoveeCustomEventError || ev == SAVED_EVENT_TIMEOUT)) {
            enter_state_prompt(app);
            consumed = true;

        } else if(saved_state == SavedStateConnecting && ev == GoveeCustomEventDisconnected) {
            // Disconnect arrived before discovery completed — treat as unreachable
            enter_state_prompt(app);
            consumed = true;

        } else if(saved_state == SavedStatePrompt && ev == SAVED_EVENT_DIALOG_REMOVE) {
            govee_bulb_cache_remove(app->cache, saved_selected_idx);
            govee_bulb_cache_save(app->cache, GOVEE_APP_CACHE_PATH);
            enter_state_idle(app);
            consumed = true;

        } else if(saved_state == SavedStatePrompt && ev == SAVED_EVENT_DIALOG_KEEP) {
            enter_state_idle(app);
            consumed = true;
        }

    } else if(event.type == SceneManagerEventTypeBack) {
        if(saved_state == SavedStateConnecting) {
            // Cancel the in-flight attempt, return to submenu
            cancel_connect_timer();
            if(govee_central_is_connected(app->central)) {
                govee_central_disconnect(app->central);
            }
            enter_state_idle(app);
            consumed = true;
        } else if(saved_state == SavedStatePrompt) {
            enter_state_idle(app);
            consumed = true;
        }
        // saved_state == Idle: let scene manager pop back to welcome/root
    }

    return consumed;
}

void govee_scene_saved_on_exit(void* ctx) {
    GoveeH6006App* app = ctx;
    if(saved_connect_timer) {
        furi_timer_stop(saved_connect_timer);
        furi_timer_free(saved_connect_timer);
        saved_connect_timer = NULL;
    }
    saved_state = SavedStateIdle;
    submenu_reset(app->submenu);
    popup_reset(app->popup);
    dialog_ex_reset(app->dialog_ex);
}
