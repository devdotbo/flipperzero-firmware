#pragma once

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/widget.h>
#include <gui/modules/popup.h>
#include <gui/modules/dialog_ex.h>
#include <gui/modules/text_input.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <dialogs/dialogs.h>

#include "protocol/govee_h6006.h"
#include "ble/govee_central.h"
#include "storage/bulb_cache.h"

#define GOVEE_APP_SCAN_MAX        16
#define GOVEE_APP_CACHE_PATH      "/ext/apps_data/govee_h6006/bulbs.json"
#define GOVEE_APP_NAME_PREFIX     "ihoment_H6006"
#define GOVEE_APP_KEEPALIVE_MS    10000
#define GOVEE_APP_SCAN_TIMEOUT_MS 15000

typedef enum {
    GoveeSceneWelcome,
    GoveeSceneScan,
    GoveeSceneControl,
    GoveeSceneLightshow,
    GoveeSceneSaved,
    GoveeSceneCount,
} GoveeScene;

typedef enum {
    GoveeViewSubmenu,
    GoveeViewVariableItemList,
    GoveeViewWidget,
    GoveeViewPopup,
    GoveeViewDialogEx,
    GoveeViewScan,
    GoveeViewRgbPicker,
    GoveeViewCount,
} GoveeView;

typedef enum {
    GoveeCustomEventScanResult = 0x1000,
    GoveeCustomEventScanTimeout,
    GoveeCustomEventConnected,
    GoveeCustomEventDisconnected,
    GoveeCustomEventDiscoveryComplete,
    GoveeCustomEventWriteComplete,
    GoveeCustomEventError,
} GoveeCustomEvent;

typedef struct {
    uint8_t addr_type;
    uint8_t addr[6];
    int8_t rssi;
    char name[32];
} GoveeScanEntry;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    NotificationApp* notifications;
    Storage* storage;
    DialogsApp* dialogs;

    Submenu* submenu;
    VariableItemList* var_item_list;
    Widget* widget;
    Popup* popup;
    DialogEx* dialog_ex;

    GoveeCentral* central;
    GoveeBulbCache* cache;

    GoveeScanEntry scan_results[GOVEE_APP_SCAN_MAX];
    size_t scan_count;
    int selected_bulb;

    bool power_on;
    uint8_t brightness;
    uint8_t rgb[3];
    uint16_t color_temp;
    uint8_t rgb_picker_hue;
    uint8_t rgb_picker_sat;
    uint8_t lightshow_phase;

    FuriTimer* keepalive_timer;
    FuriTimer* scan_timer;
    FuriTimer* lightshow_timer;
} GoveeH6006App;

void govee_app_post_custom_event(GoveeH6006App* app, GoveeCustomEvent event);
