#include "../govee_h6006_app.h"
#include "scenes.h"
#include <furi_hal.h>
#include <string.h>

#define TAG "GoveeSceneScan"

// Custom event sub-values used locally within this scene
#define SCAN_EVENT_RETRY 0x01

static void scan_submenu_callback(void* context, uint32_t index) {
    GoveeH6006App* app = context;
    // encode index as custom event value: use high bits to distinguish
    view_dispatcher_send_custom_event(app->view_dispatcher, (uint32_t)(0x2000 | (index & 0xFF)));
}

static void scan_timeout_callback(void* context) {
    GoveeH6006App* app = context;
    govee_app_post_custom_event(app, GoveeCustomEventScanTimeout);
}

static void scan_no_results_popup_callback(void* context) {
    GoveeH6006App* app = context;
    // Trigger a retry: go back to scan scene
    view_dispatcher_send_custom_event(app->view_dispatcher, SCAN_EVENT_RETRY);
}

void govee_scene_scan_on_enter(void* ctx) {
    GoveeH6006App* app = ctx;
    furi_assert(app);

    // Reset scan results
    app->scan_count = 0;
    app->selected_bulb = -1;

    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Scanning...");

    // Start scan timeout timer
    if(!app->scan_timer) {
        app->scan_timer =
            furi_timer_alloc(scan_timeout_callback, FuriTimerTypeOnce, app);
    }
    furi_timer_start(app->scan_timer, GOVEE_APP_SCAN_TIMEOUT_MS);

    govee_central_scan_start(app->central);

    view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewSubmenu);
}

bool govee_scene_scan_on_event(void* ctx, SceneManagerEvent event) {
    GoveeH6006App* app = ctx;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        uint32_t ev = event.event;

        if(ev == GoveeCustomEventScanResult) {
            // Rebuild submenu with all known results so far
            submenu_reset(app->submenu);
            submenu_set_header(app->submenu, "Scanning...");
            for(size_t i = 0; i < app->scan_count; i++) {
                // Format: "name RSSI"
                static char label[48];
                snprintf(
                    label,
                    sizeof(label),
                    "%s %ddBm",
                    app->scan_results[i].name,
                    (int)app->scan_results[i].rssi);
                submenu_add_item(app->submenu, label, (uint32_t)i, scan_submenu_callback, app);
            }
            consumed = true;

        } else if(ev == GoveeCustomEventScanTimeout) {
            // Stop scan
            govee_central_scan_stop(app->central);
            furi_timer_stop(app->scan_timer);

            if(app->scan_count == 0) {
                // Show "no results" popup
                popup_reset(app->popup);
                popup_set_header(app->popup, "No Bulbs Found", 64, 10, AlignCenter, AlignTop);
                popup_set_text(
                    app->popup,
                    "No Govee H6006 bulbs\nfound. Press OK to retry.",
                    64,
                    32,
                    AlignCenter,
                    AlignCenter);
                popup_set_context(app->popup, app);
                popup_set_callback(app->popup, scan_no_results_popup_callback);
                popup_set_timeout(app->popup, 4000);
                popup_enable_timeout(app->popup);
                view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewPopup);
            } else {
                // Update header to show scan is done
                submenu_set_header(app->submenu, "Select Bulb:");
                view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewSubmenu);
            }
            consumed = true;

        } else if(ev == SCAN_EVENT_RETRY) {
            // Retry: re-enter this scene
            scene_manager_next_scene(app->scene_manager, GoveeSceneScan);
            consumed = true;

        } else if((ev & 0xF000) == 0x2000) {
            // Submenu item selected
            uint32_t idx = ev & 0xFF;
            if(idx < app->scan_count) {
                app->selected_bulb = (int)idx;

                // Stop scan and timer
                govee_central_scan_stop(app->central);
                if(app->scan_timer) furi_timer_stop(app->scan_timer);

                // Disconnect any existing connection
                if(govee_central_is_connected(app->central)) {
                    govee_central_disconnect(app->central);
                }

                // Initiate connection
                const GoveeScanEntry* entry = &app->scan_results[idx];
                memcpy(app->current_addr, entry->addr, 6);
                app->current_addr_type = entry->addr_type;
                app->has_current_addr = true;
                govee_central_connect(app->central, entry->addr, entry->addr_type);

                // Push control scene - it will wait for GoveeCustomEventConnected
                scene_manager_next_scene(app->scene_manager, GoveeSceneControl);
            }
            consumed = true;
        }

    } else if(event.type == SceneManagerEventTypeBack) {
        govee_central_scan_stop(app->central);
        if(app->scan_timer) furi_timer_stop(app->scan_timer);
        consumed = false; // let scene manager handle back (exit app)
    }

    return consumed;
}

void govee_scene_scan_on_exit(void* ctx) {
    GoveeH6006App* app = ctx;
    govee_central_scan_stop(app->central);
    if(app->scan_timer) {
        furi_timer_stop(app->scan_timer);
    }
    submenu_reset(app->submenu);
    popup_reset(app->popup);
}
