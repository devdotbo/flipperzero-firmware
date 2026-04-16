#include "../govee_h6006_app.h"
#include "scenes.h"

#define TAG "GoveeSceneWelcome"

static void welcome_popup_callback(void* context) {
    GoveeH6006App* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, 0);
}

void govee_scene_welcome_on_enter(void* ctx) {
    GoveeH6006App* app = ctx;
    furi_assert(app);

    if(!govee_central_available()) {
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
        if(!govee_central_available()) {
            // BLE not available - exit app
            scene_manager_stop(app->scene_manager);
            view_dispatcher_stop(app->view_dispatcher);
        } else {
            scene_manager_next_scene(app->scene_manager, GoveeSceneScan);
        }
        consumed = true;
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
}
