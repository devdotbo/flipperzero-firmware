#include "../govee_h6006_app.h"
#include "scenes.h"

#define TAG "GoveeSceneControl"

// Item indices in the VariableItemList
typedef enum {
    ControlItemPower = 0,
    ControlItemBrightness,
    ControlItemCT,
    ControlItemRgb,
    ControlItemLightshow,
    ControlItemSaveFavorite,
    ControlItemSyncAll,
    ControlItemDisconnect,
    ControlItemCount,
} ControlItem;

// Custom event sub-values (local to control scene)
#define CTRL_EVENT_SAVED_POPUP_DONE      0x3001
#define CTRL_EVENT_DISCONNECT_POPUP_DONE 0x3002
#define CTRL_EVENT_ENTER_ITEM            0x3100 // | (item_index & 0xFF)

static const char* power_text[2] = {"Off", "On"};

// ---------------------------------------------------------------------------
// Return helpers
// ---------------------------------------------------------------------------

static void return_to_prior_scene(GoveeH6006App* app) {
    const uint32_t candidates[] = {GoveeSceneSaved, GoveeSceneScan, GoveeSceneWelcome};
    for(size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if(scene_manager_search_and_switch_to_previous_scene(
               app->scene_manager, candidates[i])) {
            return;
        }
    }
    scene_manager_stop(app->scene_manager);
    view_dispatcher_stop(app->view_dispatcher);
}

// ---------------------------------------------------------------------------
// Keepalive timer
// ---------------------------------------------------------------------------

static void keepalive_timer_callback(void* context) {
    GoveeH6006App* app = context;
    if(govee_central_is_connected(app->central)) {
        govee_central_keepalive_start(app->central, GOVEE_APP_KEEPALIVE_MS);
    }
}

// ---------------------------------------------------------------------------
// VariableItemList callbacks
// ---------------------------------------------------------------------------

static void control_power_change_callback(VariableItem* item) {
    GoveeH6006App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->power_on = (idx == 1);
    variable_item_set_current_value_text(item, power_text[idx]);
    govee_central_send_power(app->central, app->power_on);
}

static void control_brightness_change_callback(VariableItem* item) {
    GoveeH6006App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    // 0..20 -> 0..100 in steps of 5
    app->brightness = idx * 5;
    static char buf[8];
    snprintf(buf, sizeof(buf), "%u%%", app->brightness);
    variable_item_set_current_value_text(item, buf);
    govee_central_send_brightness(app->central, app->brightness);
}

static void control_ct_change_callback(VariableItem* item) {
    GoveeH6006App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    // 2700..6500K in steps of 200K -> 19 steps
    app->color_temp = (uint16_t)(2700 + idx * 200);
    static char buf[12];
    snprintf(buf, sizeof(buf), "%uK", app->color_temp);
    variable_item_set_current_value_text(item, buf);
    govee_central_send_ct(app->central, app->color_temp);
}

static void control_enter_callback(void* context, uint32_t index) {
    GoveeH6006App* app = context;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, (uint32_t)(CTRL_EVENT_ENTER_ITEM | (index & 0xFF)));
}

static void saved_popup_callback(void* context) {
    GoveeH6006App* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, CTRL_EVENT_SAVED_POPUP_DONE);
}

static void disconnect_popup_callback(void* context) {
    GoveeH6006App* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, CTRL_EVENT_DISCONNECT_POPUP_DONE);
}

// ---------------------------------------------------------------------------
// Auto-cache on successful discovery
// ---------------------------------------------------------------------------

static void auto_cache_on_discovery(GoveeH6006App* app) {
    if(app->selected_bulb < 0 || (size_t)app->selected_bulb >= app->scan_count) {
        // Either cache-originated (already in cache) or no valid scan entry. Skip.
        return;
    }
    const GoveeScanEntry* e = &app->scan_results[app->selected_bulb];
    if(govee_bulb_cache_upsert(app->cache, e->name, e->addr, e->addr_type)) {
        govee_bulb_cache_save(app->cache, GOVEE_APP_CACHE_PATH);
    }
}

// ---------------------------------------------------------------------------
// Scene handlers
// ---------------------------------------------------------------------------

void govee_scene_control_on_enter(void* ctx) {
    GoveeH6006App* app = ctx;
    furi_assert(app);

    VariableItemList* vil = app->var_item_list;
    variable_item_list_reset(vil);

    VariableItem* item;

    // Power toggle
    item = variable_item_list_add(vil, "Power", 2, control_power_change_callback, app);
    variable_item_set_current_value_index(item, app->power_on ? 1 : 0);
    variable_item_set_current_value_text(item, power_text[app->power_on ? 1 : 0]);

    // Brightness 0..100 step 5 -> 21 values
    item = variable_item_list_add(vil, "Brightness", 21, control_brightness_change_callback, app);
    uint8_t bri_idx = app->brightness / 5;
    if(bri_idx > 20) bri_idx = 20;
    variable_item_set_current_value_index(item, bri_idx);
    static char bri_buf[8];
    snprintf(bri_buf, sizeof(bri_buf), "%u%%", app->brightness);
    variable_item_set_current_value_text(item, bri_buf);

    // Color temp 2700..6500 step 200 -> 19 steps
    item = variable_item_list_add(vil, "Color Temp", 19, control_ct_change_callback, app);
    uint8_t ct_idx = (uint8_t)((app->color_temp - 2700) / 200);
    if(ct_idx > 18) ct_idx = 18;
    variable_item_set_current_value_index(item, ct_idx);
    static char ct_buf[12];
    snprintf(ct_buf, sizeof(ct_buf), "%uK", app->color_temp);
    variable_item_set_current_value_text(item, ct_buf);

    // RGB Picker (enter navigates to view)
    item = variable_item_list_add(vil, "RGB Picker", 1, NULL, app);
    variable_item_set_current_value_text(item, ">");

    // Lightshow
    item = variable_item_list_add(vil, "Lightshow", 1, NULL, app);
    variable_item_set_current_value_text(item, ">");

    // Save to Favorites (manual)
    item = variable_item_list_add(vil, "Save Favorite", 1, NULL, app);
    variable_item_set_current_value_text(item, "");

    // Sync current state to all saved bulbs (skips the connected one)
    item = variable_item_list_add(vil, "Sync to All Saved", 1, NULL, app);
    variable_item_set_current_value_text(item, ">");

    // Disconnect
    item = variable_item_list_add(vil, "Disconnect", 1, NULL, app);
    variable_item_set_current_value_text(item, "");

    variable_item_list_set_enter_callback(vil, control_enter_callback, app);

    // Keepalive timer
    if(!app->keepalive_timer) {
        app->keepalive_timer =
            furi_timer_alloc(keepalive_timer_callback, FuriTimerTypePeriodic, app);
    }
    govee_central_keepalive_start(app->central, GOVEE_APP_KEEPALIVE_MS);
    furi_timer_start(app->keepalive_timer, GOVEE_APP_KEEPALIVE_MS);

    view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewVariableItemList);
}

bool govee_scene_control_on_event(void* ctx, SceneManagerEvent event) {
    GoveeH6006App* app = ctx;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        uint32_t ev = event.event;

        if(ev == GoveeCustomEventDiscoveryComplete) {
            auto_cache_on_discovery(app);
            consumed = true;

        } else if(ev == GoveeCustomEventDisconnected || ev == GoveeCustomEventError) {
            govee_central_keepalive_stop(app->central);
            if(app->keepalive_timer) furi_timer_stop(app->keepalive_timer);

            popup_reset(app->popup);
            popup_set_header(app->popup, "Disconnected", 64, 10, AlignCenter, AlignTop);
            popup_set_text(
                app->popup,
                "Lost connection.\nReturning.",
                64,
                32,
                AlignCenter,
                AlignCenter);
            popup_set_timeout(app->popup, 2500);
            popup_enable_timeout(app->popup);
            popup_set_context(app->popup, app);
            popup_set_callback(app->popup, disconnect_popup_callback);
            view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewPopup);
            consumed = true;

        } else if(ev == CTRL_EVENT_DISCONNECT_POPUP_DONE) {
            return_to_prior_scene(app);
            consumed = true;

        } else if(ev == CTRL_EVENT_SAVED_POPUP_DONE) {
            // Return to control after transient "Saved!" popup
            view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewVariableItemList);
            consumed = true;

        } else if((ev & 0xFF00) == (CTRL_EVENT_ENTER_ITEM & 0xFF00)) {
            uint32_t item_idx = ev & 0xFF;

            if(item_idx == ControlItemRgb) {
                view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewRgbPicker);
                consumed = true;
            } else if(item_idx == ControlItemLightshow) {
                scene_manager_next_scene(app->scene_manager, GoveeSceneLightshow);
                consumed = true;
            } else if(item_idx == ControlItemSaveFavorite) {
                if(app->selected_bulb >= 0 && app->selected_bulb < (int)app->scan_count) {
                    const GoveeScanEntry* e = &app->scan_results[app->selected_bulb];
                    govee_bulb_cache_upsert(app->cache, e->name, e->addr, e->addr_type);
                    govee_bulb_cache_save(app->cache, GOVEE_APP_CACHE_PATH);
                }
                popup_reset(app->popup);
                popup_set_header(app->popup, "Saved!", 64, 28, AlignCenter, AlignCenter);
                popup_set_timeout(app->popup, 1500);
                popup_enable_timeout(app->popup);
                popup_set_context(app->popup, app);
                popup_set_callback(app->popup, saved_popup_callback);
                view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewPopup);
                consumed = true;
            } else if(item_idx == ControlItemSyncAll) {
                // Stop keepalive while the group run holds the central link
                govee_central_keepalive_stop(app->central);
                if(app->keepalive_timer) furi_timer_stop(app->keepalive_timer);
                scene_manager_next_scene(app->scene_manager, GoveeSceneGroupApply);
                consumed = true;
            } else if(item_idx == ControlItemDisconnect) {
                govee_central_keepalive_stop(app->central);
                if(app->keepalive_timer) furi_timer_stop(app->keepalive_timer);
                govee_central_disconnect(app->central);
                app->has_current_addr = false;
                return_to_prior_scene(app);
                consumed = true;
            }
        }

    } else if(event.type == SceneManagerEventTypeBack) {
        consumed = false;
    }

    return consumed;
}

void govee_scene_control_on_exit(void* ctx) {
    GoveeH6006App* app = ctx;
    govee_central_keepalive_stop(app->central);
    if(app->keepalive_timer) {
        furi_timer_stop(app->keepalive_timer);
        furi_timer_free(app->keepalive_timer);
        app->keepalive_timer = NULL;
    }
    variable_item_list_reset(app->var_item_list);
    popup_reset(app->popup);
}
