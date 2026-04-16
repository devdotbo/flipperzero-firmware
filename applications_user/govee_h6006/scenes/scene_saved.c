#include "../govee_h6006_app.h"
#include "scenes.h"

#define TAG "GoveeSceneSaved"

#define SAVED_EVENT_CONNECT   0x4000
#define SAVED_EVENT_DELETE_OK 0x4001

static uint32_t saved_selected_idx = 0;

static void saved_submenu_callback(void* context, uint32_t index) {
    GoveeH6006App* app = context;
    saved_selected_idx = index;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, (uint32_t)(SAVED_EVENT_CONNECT | (index & 0xFF)));
}

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

void govee_scene_saved_on_enter(void* ctx) {
    GoveeH6006App* app = ctx;
    furi_assert(app);

    saved_selected_idx = 0;
    rebuild_submenu(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewSubmenu);
}

bool govee_scene_saved_on_event(void* ctx, SceneManagerEvent event) {
    GoveeH6006App* app = ctx;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        uint32_t ev = event.event;

        if((ev & 0xFF00) == (SAVED_EVENT_CONNECT & 0xFF00) && (ev & 0xF000) == 0x4000) {
            // Connect to cached bulb
            uint32_t idx = ev & 0xFF;
            const GoveeBulbCacheEntry* e = govee_bulb_cache_get(app->cache, idx);
            if(e) {
                saved_selected_idx = idx;
                // Disconnect any existing
                if(govee_central_is_connected(app->central)) {
                    govee_central_disconnect(app->central);
                }
                govee_central_connect(app->central, e->addr, e->addr_type);
                app->selected_bulb = -1; // cached entry, no scan_results index
                scene_manager_next_scene(app->scene_manager, GoveeSceneControl);
            }
            consumed = true;

        } else if(ev == SAVED_EVENT_DELETE_OK) {
            govee_bulb_cache_remove(app->cache, saved_selected_idx);
            govee_bulb_cache_save(app->cache, GOVEE_APP_CACHE_PATH);
            rebuild_submenu(app);
            view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewSubmenu);
            consumed = true;

        }

    } else if(event.type == SceneManagerEventTypeTick) {
        consumed = false;
    }

    return consumed;
}

void govee_scene_saved_on_exit(void* ctx) {
    GoveeH6006App* app = ctx;
    submenu_reset(app->submenu);
    dialog_ex_reset(app->dialog_ex);
}
