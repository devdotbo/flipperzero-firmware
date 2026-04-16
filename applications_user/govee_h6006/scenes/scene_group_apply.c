#include "../govee_h6006_app.h"
#include "scenes.h"

#include <string.h>

#define TAG "GoveeSceneGroupApply"

#define GROUP_APPLY_EVENT_DONE_POPUP_DONE 0x6001

typedef enum {
    GroupSceneRunning,
    GroupSceneFinalizing,
} GroupSceneState;

// Scene-local state. Only one group run active at a time; scene re-entry
// clears these in on_enter.
static GroupSceneState s_state = GroupSceneRunning;
static uint32_t s_progress_idx = 0;
static uint32_t s_progress_total = 0;
static char s_progress_name[32] = {0};

// Forward decl on the app-side central callback so we can restore it after
// the group run. Defined in govee_h6006_app.c.
extern void govee_app_restore_central_callback(GoveeH6006App* app);

// ---------------------------------------------------------------------------
// Group callback: fans group events back through the view dispatcher
// so on_event can handle them on the scene thread.
// ---------------------------------------------------------------------------

static void group_cb(GoveeGroup* grp, GoveeGroupEvent ev, const void* data, void* ctx) {
    UNUSED(grp);
    GoveeH6006App* app = ctx;

    if(ev == GoveeGroupEventProgress || ev == GoveeGroupEventBulbDone ||
       ev == GoveeGroupEventBulbFailed) {
        if(data) {
            const GoveeGroupProgress* p = data;
            s_progress_idx = p->idx;
            s_progress_total = p->total;
            strlcpy(s_progress_name, p->name, sizeof(s_progress_name));
        }
    }

    uint32_t custom = 0;
    switch(ev) {
    case GoveeGroupEventProgress:     custom = GoveeCustomEventGroupProgress; break;
    case GoveeGroupEventBulbDone:     custom = GoveeCustomEventGroupBulbDone; break;
    case GoveeGroupEventBulbFailed:   custom = GoveeCustomEventGroupBulbFailed; break;
    case GoveeGroupEventAllDone:      custom = GoveeCustomEventGroupAllDone; break;
    case GoveeGroupEventAborted:      custom = GoveeCustomEventGroupAborted; break;
    }
    view_dispatcher_send_custom_event(app->view_dispatcher, custom);
}

// ---------------------------------------------------------------------------
// Popup
// ---------------------------------------------------------------------------

static void popup_done_cb(void* context) {
    GoveeH6006App* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, GROUP_APPLY_EVENT_DONE_POPUP_DONE);
}

static void render_progress_popup(GoveeH6006App* app) {
    popup_reset(app->popup);
    static char header[32];
    snprintf(
        header,
        sizeof(header),
        "Syncing %lu/%lu",
        (unsigned long)(s_progress_idx + 1),
        (unsigned long)s_progress_total);
    popup_set_header(app->popup, header, 64, 10, AlignCenter, AlignTop);
    static char body[48];
    snprintf(body, sizeof(body), "%s", s_progress_name);
    popup_set_text(app->popup, body, 64, 32, AlignCenter, AlignCenter);
    popup_set_context(app->popup, app);
    popup_disable_timeout(app->popup);
    view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewPopup);
}

static void render_final_popup(GoveeH6006App* app, bool aborted) {
    popup_reset(app->popup);
    static char header[32];
    static char body[64];

    if(aborted) {
        strlcpy(header, "Aborted", sizeof(header));
        snprintf(
            body,
            sizeof(body),
            "%lu done, %lu failed",
            (unsigned long)govee_group_done_count(app->group),
            (unsigned long)govee_group_failed_count(app->group));
    } else {
        uint32_t done = govee_group_done_count(app->group);
        uint32_t failed = govee_group_failed_count(app->group);
        snprintf(header, sizeof(header), "Synced %lu", (unsigned long)done);
        if(failed > 0) {
            snprintf(body, sizeof(body), "%lu failed", (unsigned long)failed);
        } else {
            strlcpy(body, "All bulbs updated", sizeof(body));
        }
    }

    popup_set_header(app->popup, header, 64, 10, AlignCenter, AlignTop);
    popup_set_text(app->popup, body, 64, 32, AlignCenter, AlignCenter);
    popup_set_timeout(app->popup, 1500);
    popup_enable_timeout(app->popup);
    popup_set_context(app->popup, app);
    popup_set_callback(app->popup, popup_done_cb);
    view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewPopup);
}

// ---------------------------------------------------------------------------
// Scene handlers
// ---------------------------------------------------------------------------

void govee_scene_group_apply_on_enter(void* ctx) {
    GoveeH6006App* app = ctx;
    furi_assert(app);

    s_state = GroupSceneRunning;
    s_progress_idx = 0;
    s_progress_total = 0;
    s_progress_name[0] = '\0';

    // Build command set from current app state — reflects exactly what the
    // user just set in the control scene.
    GoveeGroupCommandSet cmds = {0};
    cmds.set_power = true;
    cmds.power = app->power_on;
    cmds.set_brightness = true;
    cmds.brightness = app->brightness;
    cmds.set_ct = true;
    cmds.kelvin = app->color_temp;
    cmds.set_rgb = true;
    cmds.r = app->rgb[0];
    cmds.g = app->rgb[1];
    cmds.b = app->rgb[2];

    govee_group_set_callback(app->group, group_cb, app);

    const uint8_t* skip = app->has_current_addr ? app->current_addr : NULL;
    bool started = govee_group_apply_all(app->group, &cmds, skip);
    if(!started) {
        // No targets (empty cache minus skip) -> immediate "nothing to do".
        popup_reset(app->popup);
        popup_set_header(app->popup, "Nothing to sync", 64, 10, AlignCenter, AlignTop);
        popup_set_text(
            app->popup,
            "No other saved bulbs.",
            64,
            32,
            AlignCenter,
            AlignCenter);
        popup_set_timeout(app->popup, 1500);
        popup_enable_timeout(app->popup);
        popup_set_context(app->popup, app);
        popup_set_callback(app->popup, popup_done_cb);
        view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewPopup);
        s_state = GroupSceneFinalizing;
        return;
    }

    render_progress_popup(app);
}

bool govee_scene_group_apply_on_event(void* ctx, SceneManagerEvent event) {
    GoveeH6006App* app = ctx;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        uint32_t ev = event.event;

        if(ev == GoveeCustomEventGroupProgress || ev == GoveeCustomEventGroupBulbDone ||
           ev == GoveeCustomEventGroupBulbFailed) {
            if(s_state == GroupSceneRunning) {
                render_progress_popup(app);
            }
            consumed = true;

        } else if(ev == GoveeCustomEventGroupAllDone) {
            s_state = GroupSceneFinalizing;
            govee_app_restore_central_callback(app);
            render_final_popup(app, false);
            consumed = true;

        } else if(ev == GoveeCustomEventGroupAborted) {
            s_state = GroupSceneFinalizing;
            govee_app_restore_central_callback(app);
            render_final_popup(app, true);
            consumed = true;

        } else if(ev == GROUP_APPLY_EVENT_DONE_POPUP_DONE) {
            // Optionally restore prior connection before returning.
            if(app->has_current_addr) {
                govee_central_connect(
                    app->central, app->current_addr, app->current_addr_type);
            }
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        }

    } else if(event.type == SceneManagerEventTypeBack) {
        if(s_state == GroupSceneRunning) {
            govee_group_abort(app->group);
            // Callback will emit Aborted -> handled above
            consumed = true;
        } else {
            consumed = false; // allow pop
        }
    }

    return consumed;
}

void govee_scene_group_apply_on_exit(void* ctx) {
    GoveeH6006App* app = ctx;
    // Safety: if the scene is being torn down while a group run is still
    // active (app quit, power off), abort and restore callback.
    if(govee_group_is_running(app->group)) {
        govee_group_abort(app->group);
        govee_app_restore_central_callback(app);
    }
    popup_reset(app->popup);
    s_state = GroupSceneRunning;
}
