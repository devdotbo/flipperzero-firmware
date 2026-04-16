#include "govee_h6006_app.h"
#include "scenes/scenes.h"
#include "views/view_rgb_picker.h"
#include <string.h>
#include <bt/bt_service/bt.h>
#include <furi_hal_bt.h>

#define TAG "GoveeApp"

// ---------------------------------------------------------------------------
// Event callbacks
// ---------------------------------------------------------------------------

static bool app_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    GoveeH6006App* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool app_back_event_callback(void* context) {
    furi_assert(context);
    GoveeH6006App* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

// ---------------------------------------------------------------------------
// BLE central -> app event translation
// ---------------------------------------------------------------------------

static void on_central_event(
    GoveeCentral* gc,
    GoveeCentralEvent event,
    const void* data,
    void* context) {
    UNUSED(gc);
    GoveeH6006App* app = context;
    furi_assert(app);

    switch(event) {
    case GoveeCentralEventScanResult: {
        if(app->scan_count < GOVEE_APP_SCAN_MAX) {
            const GoveeCentralScanEntry* e = data;
            GoveeScanEntry* dst = &app->scan_results[app->scan_count];
            dst->addr_type = e->addr_type;
            memcpy(dst->addr, e->addr, 6);
            dst->rssi = e->rssi;
            strncpy(dst->name, e->name, sizeof(dst->name) - 1);
            dst->name[sizeof(dst->name) - 1] = '\0';
            app->scan_count++;
        }
        view_dispatcher_send_custom_event(
            app->view_dispatcher, (uint32_t)GoveeCustomEventScanResult);
        break;
    }
    case GoveeCentralEventScanComplete:
        view_dispatcher_send_custom_event(
            app->view_dispatcher, (uint32_t)GoveeCustomEventScanTimeout);
        break;
    case GoveeCentralEventConnected:
        view_dispatcher_send_custom_event(
            app->view_dispatcher, (uint32_t)GoveeCustomEventConnected);
        break;
    case GoveeCentralEventDisconnected:
        view_dispatcher_send_custom_event(
            app->view_dispatcher, (uint32_t)GoveeCustomEventDisconnected);
        break;
    case GoveeCentralEventDiscoveryComplete:
        view_dispatcher_send_custom_event(
            app->view_dispatcher, (uint32_t)GoveeCustomEventDiscoveryComplete);
        break;
    case GoveeCentralEventWriteComplete:
        view_dispatcher_send_custom_event(
            app->view_dispatcher, (uint32_t)GoveeCustomEventWriteComplete);
        break;
    case GoveeCentralEventError:
        view_dispatcher_send_custom_event(
            app->view_dispatcher, (uint32_t)GoveeCustomEventError);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Public helper
// ---------------------------------------------------------------------------

void govee_app_post_custom_event(GoveeH6006App* app, GoveeCustomEvent event) {
    furi_assert(app);
    view_dispatcher_send_custom_event(app->view_dispatcher, (uint32_t)event);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int32_t govee_h6006_app(void* p) {
    UNUSED(p);

    GoveeH6006App* app = malloc(sizeof(GoveeH6006App));
    furi_assert(app);
    memset(app, 0, sizeof(GoveeH6006App));

    // Open system records
    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->dialogs = furi_record_open(RECORD_DIALOGS);

    // Suspend peripheral advertising so central scan isn't rejected (err 0xC)
    Bt* bt = furi_record_open(RECORD_BT);
    bt_disconnect(bt);
    furi_delay_ms(200);
    furi_hal_bt_stop_advertising();
    furi_record_close(RECORD_BT);

    // View dispatcher + scene manager
    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&govee_scene_handlers, app);

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, app_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, app_back_event_callback);

    // GUI modules
    app->submenu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, GoveeViewSubmenu, submenu_get_view(app->submenu));

    app->var_item_list = variable_item_list_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        GoveeViewVariableItemList,
        variable_item_list_get_view(app->var_item_list));

    app->widget = widget_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, GoveeViewWidget, widget_get_view(app->widget));

    app->popup = popup_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, GoveeViewPopup, popup_get_view(app->popup));

    app->dialog_ex = dialog_ex_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, GoveeViewDialogEx, dialog_ex_get_view(app->dialog_ex));

    // RGB picker custom view (no struct field in app - managed locally)
    View* rgb_picker_view = govee_view_rgb_picker_alloc(app);
    view_dispatcher_add_view(app->view_dispatcher, GoveeViewRgbPicker, rgb_picker_view);

    // BLE and cache
    app->central = govee_central_alloc();
    govee_central_set_callback(app->central, on_central_event, app);

    app->cache = govee_bulb_cache_alloc();
    govee_bulb_cache_load(app->cache, GOVEE_APP_CACHE_PATH);

    // Default state
    app->power_on = true;
    app->brightness = 100;
    app->color_temp = 4000;
    app->rgb[0] = 255;
    app->rgb[1] = 255;
    app->rgb[2] = 255;

    FURI_LOG_I(TAG, "Starting app");

    // Attach + run
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    scene_manager_next_scene(app->scene_manager, GoveeSceneWelcome);
    view_dispatcher_run(app->view_dispatcher);

    // ---------------------------------------------------------------------------
    // Teardown
    // ---------------------------------------------------------------------------
    FURI_LOG_I(TAG, "Teardown");

    // Stop any running timers that scenes may have left (safety)
    if(app->scan_timer) {
        furi_timer_stop(app->scan_timer);
        furi_timer_free(app->scan_timer);
        app->scan_timer = NULL;
    }
    if(app->keepalive_timer) {
        furi_timer_stop(app->keepalive_timer);
        furi_timer_free(app->keepalive_timer);
        app->keepalive_timer = NULL;
    }
    if(app->lightshow_timer) {
        furi_timer_stop(app->lightshow_timer);
        furi_timer_free(app->lightshow_timer);
        app->lightshow_timer = NULL;
    }

    govee_central_free(app->central);

    govee_bulb_cache_save(app->cache, GOVEE_APP_CACHE_PATH);
    govee_bulb_cache_free(app->cache);

    view_dispatcher_remove_view(app->view_dispatcher, GoveeViewRgbPicker);
    govee_view_rgb_picker_free(rgb_picker_view);

    view_dispatcher_remove_view(app->view_dispatcher, GoveeViewDialogEx);
    dialog_ex_free(app->dialog_ex);

    view_dispatcher_remove_view(app->view_dispatcher, GoveeViewPopup);
    popup_free(app->popup);

    view_dispatcher_remove_view(app->view_dispatcher, GoveeViewWidget);
    widget_free(app->widget);

    view_dispatcher_remove_view(app->view_dispatcher, GoveeViewVariableItemList);
    variable_item_list_free(app->var_item_list);

    view_dispatcher_remove_view(app->view_dispatcher, GoveeViewSubmenu);
    submenu_free(app->submenu);

    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);

    // Restore peripheral advertising for other Flipper BLE features
    Bt* bt_teardown = furi_record_open(RECORD_BT);
    bt_profile_restore_default(bt_teardown);
    furi_record_close(RECORD_BT);

    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);

    free(app);
    return 0;
}
