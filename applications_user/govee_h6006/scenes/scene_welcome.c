#include "../govee_h6006_app.h"
#include "scenes.h"

#define TAG "GoveeSceneWelcome"

#define WELCOME_EVENT_GOTO_SAVED 0x5001
#define WELCOME_EVENT_GOTO_SCAN  0x5002
#define WELCOME_EVENT_EXIT       0x5003
#define WELCOME_EVENT_POPUP_DONE 0x5004

static void welcome_popup_callback(void* context) {
    GoveeH6006App* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, WELCOME_EVENT_POPUP_DONE);
}

static void welcome_submenu_callback(void* context, uint32_t index) {
    GoveeH6006App* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void govee_scene_welcome_on_enter(void* ctx) {
    GoveeH6006App* app = ctx;
    furi_assert(app);

    if(!govee_central_available()) {
        popup_reset(app->popup);
        popup_set_header(app->popup, "BLE Not Available", 64, 10, AlignCenter, AlignTop);
        popup_set_text(
            app->popup,
            "Requires BLE Full stack.\nCheck BT settings.",
            64,
            32,
            AlignCenter,
            AlignCenter);
        popup_set_timeout(app->popup, 3000);
        popup_set_context(app->popup, app);
        popup_set_callback(app->popup, welcome_popup_callback);
        popup_enable_timeout(app->popup);
        view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewPopup);
        return;
    }

    size_t saved = govee_bulb_cache_count(app->cache);
    if(saved > 0) {
        submenu_reset(app->submenu);
        submenu_set_header(app->submenu, "Govee H6006");

        static char saved_label[32];
        snprintf(saved_label, sizeof(saved_label), "Saved Bulbs (%u)", (unsigned)saved);
        submenu_add_item(
            app->submenu, saved_label, WELCOME_EVENT_GOTO_SAVED, welcome_submenu_callback, app);
        submenu_add_item(
            app->submenu, "Scan for new", WELCOME_EVENT_GOTO_SCAN, welcome_submenu_callback, app);
        submenu_add_item(
            app->submenu, "Exit", WELCOME_EVENT_EXIT, welcome_submenu_callback, app);

        view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewSubmenu);
        return;
    }

    popup_reset(app->popup);
    popup_set_header(app->popup, "Govee H6006", 64, 10, AlignCenter, AlignTop);
    popup_set_text(
        app->popup,
        "BLE LED controller\nPress OK to scan",
        64,
        32,
        AlignCenter,
        AlignCenter);
    popup_set_context(app->popup, app);
    popup_set_callback(app->popup, welcome_popup_callback);

    view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewPopup);
}

bool govee_scene_welcome_on_event(void* ctx, SceneManagerEvent event) {
    GoveeH6006App* app = ctx;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        uint32_t ev = event.event;

        if(!govee_central_available()) {
            scene_manager_stop(app->scene_manager);
            view_dispatcher_stop(app->view_dispatcher);
            consumed = true;
        } else if(ev == WELCOME_EVENT_GOTO_SAVED) {
            scene_manager_next_scene(app->scene_manager, GoveeSceneSaved);
            consumed = true;
        } else if(ev == WELCOME_EVENT_GOTO_SCAN || ev == WELCOME_EVENT_POPUP_DONE) {
            scene_manager_next_scene(app->scene_manager, GoveeSceneScan);
            consumed = true;
        } else if(ev == WELCOME_EVENT_EXIT) {
            scene_manager_stop(app->scene_manager);
            view_dispatcher_stop(app->view_dispatcher);
            consumed = true;
        } else {
            scene_manager_next_scene(app->scene_manager, GoveeSceneScan);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        scene_manager_stop(app->scene_manager);
        view_dispatcher_stop(app->view_dispatcher);
        consumed = true;
    }

    return consumed;
}

void govee_scene_welcome_on_exit(void* ctx) {
    GoveeH6006App* app = ctx;
    popup_reset(app->popup);
    submenu_reset(app->submenu);
}
