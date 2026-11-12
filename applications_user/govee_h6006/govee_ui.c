/**
 * @file govee_ui.c
 * Screens: scan, Home, Lamps, lamp detail, colour picker,
 * lightshow, notice. App thread only. View callbacks that would rebuild their
 * own view post a deferred action to the inbox instead.
 */
#include "govee_h6006_app.h"
#include "views/view_rgb_picker.h"

#include <stdio.h>
#include <string.h>

#define BRI_STEPS    (20u) /* 5 % steps */
#define KELVIN_STEP  (100u)
#define KELVIN_STEPS ((GOVEE_KELVIN_MAX - GOVEE_KELVIN_MIN) / KELVIN_STEP + 1u)

typedef struct {
    uint32_t rgb;
    const char* name;
} ShowColour;

static const ShowColour show_palette[] = {
    {0xFF0000, "Red"},
    {0xFF8000, "Orange"},
    {0xFFFF00, "Yellow"},
    {0x00FF00, "Green"},
    {0x00FFFF, "Cyan"},
    {0x0000FF, "Blue"},
    {0xB400FF, "Violet"},
    {0xFF00B4, "Pink"},
};
#define SHOW_COUNT (sizeof(show_palette) / sizeof(show_palette[0]))

static const char* const off_on[] = {"Off", "On"};

static void post_ui(GoveeH6006App* app, GoveeUiAction action, uint32_t arg) {
    GoveeMsg msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = GoveeMsgUi;
    msg.ui.action = (uint8_t)action;
    msg.ui.arg = arg;
    govee_app_post(app, &msg);
}

static uint8_t targeted_count(const GoveeH6006App* app) {
    uint8_t n = app->registry->count;
    if(n == 0) return 0;
    return (app->target != 0 && app->target <= n) ? 1u : n;
}

static const char* outcome_short(uint8_t o) {
    switch(o) {
    case GoveeOutcomeSubmitted:
    case GoveeOutcomeUnchanged:
        return "ok";
    case GoveeOutcomeConnectFailed:
        return "conn";
    case GoveeOutcomeConnectTimeout:
        return "tmo";
    case GoveeOutcomeCancelFailed:
        return "cncl";
    case GoveeOutcomeDiscoveryFailed:
        return "disc";
    case GoveeOutcomeWriteRejected:
        return "wr";
    case GoveeOutcomeLinkLost:
        return "lost";
    case GoveeOutcomeAborted:
        return "abrt";
    default:
        return "-";
    }
}

static const GoveeEngineLamp* engine_lamp(const GoveeH6006App* app, uint8_t index0) {
    uint8_t ei = govee_engine_find_lamp(app->engine, &app->registry->lamps[index0].id);
    return (ei == GOVEE_LAMP_NONE) ? NULL : govee_engine_lamp(app->engine, ei);
}

static const char* short_name(const char* name) {
    const char* prefix = "ihoment_";
    size_t n = strlen(prefix);
    return (strncmp(name, prefix, n) == 0 && name[n] != '\0') ? name + n : name;
}

/* ------------------------------------------------------------------------- */
/* Home                                                                       */
/* ------------------------------------------------------------------------- */

static void home_update_items(GoveeH6006App* app) {
    const GoveeDesired* d = &app->desired;
    char* t = app->value_text;
    size_t ts = sizeof(app->value_text);

    variable_item_set_current_value_index(app->row[GoveeHomePower], d->power ? 1 : 0);
    variable_item_set_current_value_text(app->row[GoveeHomePower], off_on[d->power ? 1 : 0]);

    uint8_t bi = (uint8_t)((d->brightness > 0 ? d->brightness - 1u : 0u) / 5u);
    if(bi >= BRI_STEPS) bi = BRI_STEPS - 1u;
    variable_item_set_current_value_index(app->row[GoveeHomeBrightness], bi);
    snprintf(t, ts, "%u%%", d->brightness);
    variable_item_set_current_value_text(app->row[GoveeHomeBrightness], t);

    variable_item_set_current_value_index(app->row[GoveeHomeMode], d->mode == GoveeModeColour);
    variable_item_set_current_value_text(
        app->row[GoveeHomeMode], d->mode == GoveeModeColour ? "Colour" : "White");

    uint8_t ki = (uint8_t)((d->kelvin - GOVEE_KELVIN_MIN) / KELVIN_STEP);
    if(ki >= KELVIN_STEPS) ki = KELVIN_STEPS - 1u;
    variable_item_set_current_value_index(app->row[GoveeHomeKelvin], ki);
    snprintf(t, ts, "%uK", d->kelvin);
    variable_item_set_current_value_text(app->row[GoveeHomeKelvin], t);

    snprintf(t, ts, "%06lX", d->rgb & 0xFFFFFFu);
    variable_item_set_current_value_text(app->row[GoveeHomeColour], t);

    variable_item_set_current_value_index(app->row[GoveeHomeLightshow], app->lightshow ? 1 : 0);
    variable_item_set_current_value_text(app->row[GoveeHomeLightshow], off_on[app->lightshow ? 1 : 0]);

    uint8_t n = app->registry->count;
    if(app->target == 0 || app->target > n) {
        snprintf(t, ts, "All %u", n);
    } else {
        snprintf(t, ts, "Only %u/%u", app->target, n);
    }
    variable_item_set_current_value_text(app->row[GoveeHomeLamps], t);

    variable_item_set_current_value_text(app->row[GoveeHomeFind], "");

    const GoveeEngine* e = app->engine;
    if(govee_engine_state(e) == GoveeEngineFaulted) {
        snprintf(t, ts, "fault");
    } else if(e->in_pass) {
        snprintf(t, ts, "sending");
    } else if(app->engine_sync_pending) {
        snprintf(t, ts, "held");
    } else if(govee_engine_revision(e) == 0) {
        snprintf(t, ts, "-");
    } else {
        uint8_t ok = 0, tg = 0;
        uint32_t rev = govee_engine_revision(e);
        for(uint8_t i = 0; i < e->lamp_count; i++) {
            const GoveeEngineLamp* l = govee_engine_lamp(e, i);
            if(!l || !l->targeted) continue;
            tg++;
            if(l->reached_rev == rev) ok++;
        }
        snprintf(t, ts, "%u/%u ok", ok, tg);
    }
    variable_item_set_current_value_text(app->row[GoveeHomeResult], t);
}

/* The GUI thread draws Home under its model lock (recursive mutex), so the
 * rows change under the same lock, and the commit requests a redraw. */
static void home_update(GoveeH6006App* app) {
    View* view = variable_item_list_get_view(app->home);
    view_get_model(view);
    home_update_items(app);
    view_commit_model(view, true);
}

static void on_power(VariableItem* item) {
    GoveeH6006App* app = variable_item_get_context(item);
    app->desired.power = variable_item_get_current_value_index(item) != 0;
    govee_link_set_desired(app);
    govee_ui_refresh(app);
}

static void on_brightness(VariableItem* item) {
    GoveeH6006App* app = variable_item_get_context(item);
    app->desired.brightness = (uint8_t)((variable_item_get_current_value_index(item) + 1u) * 5u);
    govee_link_set_desired(app);
    govee_ui_refresh(app);
}

static void on_mode(VariableItem* item) {
    GoveeH6006App* app = variable_item_get_context(item);
    if(app->lightshow) govee_ui_lightshow(app, false);
    app->desired.mode = variable_item_get_current_value_index(item) ? GoveeModeColour :
                                                                      GoveeModeWhite;
    govee_link_set_desired(app);
    govee_ui_refresh(app);
}

static void on_kelvin(VariableItem* item) {
    GoveeH6006App* app = variable_item_get_context(item);
    if(app->lightshow) govee_ui_lightshow(app, false);
    app->desired.kelvin =
        (uint16_t)(GOVEE_KELVIN_MIN + variable_item_get_current_value_index(item) * KELVIN_STEP);
    app->desired.mode = GoveeModeWhite;
    govee_link_set_desired(app);
    govee_ui_refresh(app);
}

static void on_lightshow(VariableItem* item) {
    GoveeH6006App* app = variable_item_get_context(item);
    govee_ui_lightshow(app, variable_item_get_current_value_index(item) != 0);
    govee_ui_refresh(app);
}

static void on_static(VariableItem* item) {
    UNUSED(item);
}

static void on_home_enter(void* context, uint32_t index) {
    GoveeH6006App* app = context;
    switch(index) {
    case GoveeHomePower:
    case GoveeHomeBrightness:
    case GoveeHomeMode:
    case GoveeHomeKelvin:
        /* OK on a value row sends the current state again to the targeted lamps */
        govee_link_apply_again(app);
        break;
    case GoveeHomeColour:
        govee_ui_show(app, GoveeScreenColour);
        break;
    case GoveeHomeLamps:
        govee_ui_show(app, GoveeScreenLamps);
        break;
    case GoveeHomeFind:
        govee_link_request_scan(app, false);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Lamps and lamp detail                                                     */
/* ------------------------------------------------------------------------- */

static void on_lamps_select(void* context, uint32_t index) {
    post_ui(context, GoveeUiLampsSelect, index);
}

static void lamps_build(GoveeH6006App* app) {
    uint32_t selected = submenu_get_selected_item(app->lamps);
    submenu_reset(app->lamps);
    uint8_t n = app->registry->count;
    char* t = app->lamp_text;
    size_t ts = sizeof(app->lamp_text);
    if(app->target == 0 || app->target > n) {
        snprintf(app->value_text, sizeof(app->value_text), "Lamps %u, target all", n);
    } else {
        snprintf(app->value_text, sizeof(app->value_text), "Lamps %u, only %u", n, app->target);
    }
    submenu_set_header(app->lamps, app->value_text);
    submenu_add_item(
        app->lamps, (app->target == 0) ? "* All lamps" : "  All lamps", 0, on_lamps_select, app);
    for(uint8_t i = 0; i < n; i++) {
        const GoveeEngineLamp* l = engine_lamp(app, i);
        snprintf(
            t,
            ts,
            "%c%u %s %s",
            (app->target == i + 1u) ? '*' : ' ',
            i + 1u,
            outcome_short(l ? l->outcome : GoveeOutcomeNone),
            short_name(app->registry->lamps[i].name));
        submenu_add_item(app->lamps, t, i + 1u, on_lamps_select, app);
    }
    submenu_set_selected_item(app->lamps, selected <= n ? selected : 0);
}

static void on_dialog_result(DialogExResult result, void* context) {
    post_ui(context, GoveeUiLampDialog, (uint32_t)result);
}

static void lamp_build(GoveeH6006App* app) {
    uint8_t i = app->lamp_sel;
    if(i >= app->registry->count) return;
    const GoveeRegistryLamp* r = &app->registry->lamps[i];
    const GoveeEngineLamp* l = engine_lamp(app, i);
    char addr[18];
    govee_registry_format_addr(r->id.addr, addr);
    if(l && l->outcome != GoveeOutcomeNone) {
        snprintf(
            app->lamp_text,
            sizeof(app->lamp_text),
            "%s\n%s %lums\nc%lu d%lu w%lu x%lu",
            addr,
            govee_engine_outcome_name((GoveeOutcome)l->outcome),
            l->times.total_ms,
            l->times.connect_ms,
            l->times.discover_ms,
            l->times.write_ms,
            l->times.disconnect_ms);
    } else {
        snprintf(app->lamp_text, sizeof(app->lamp_text), "%s\nnot sent yet", addr);
    }
    dialog_ex_reset(app->dialog);
    dialog_ex_set_header(
        app->dialog, r->name[0] ? short_name(r->name) : "Lamp", 64, 0, AlignCenter, AlignTop);
    dialog_ex_set_text(app->dialog, app->lamp_text, 0, 12, AlignLeft, AlignTop);
    dialog_ex_set_left_button_text(app->dialog, "Remove");
    dialog_ex_set_center_button_text(app->dialog, "All");
    dialog_ex_set_right_button_text(app->dialog, "Only");
    dialog_ex_set_result_callback(app->dialog, on_dialog_result);
    dialog_ex_set_context(app->dialog, app);
}

static void notice_build(GoveeH6006App* app) {
    dialog_ex_reset(app->dialog);
    dialog_ex_set_header(app->dialog, "Govee H6006", 64, 0, AlignCenter, AlignTop);
    dialog_ex_set_text(app->dialog, app->notice, 64, 32, AlignCenter, AlignCenter);
    dialog_ex_set_center_button_text(app->dialog, "OK");
    dialog_ex_set_result_callback(app->dialog, on_dialog_result);
    dialog_ex_set_context(app->dialog, app);
}

/* Where a notice returns to: a screen that still makes sense. */
static GoveeScreen notice_back_screen(const GoveeH6006App* app) {
    switch(app->notice_return) {
    case GoveeScreenLamp:
        return GoveeScreenLamps;
    case GoveeScreenLightshow:
        return app->lightshow ? GoveeScreenLightshow : GoveeScreenHome;
    case GoveeScreenScan:
        return (app->scan_state == GoveeScanIdle) ? GoveeScreenHome : GoveeScreenScan;
    case GoveeScreenNone:
    case GoveeScreenNotice:
        return GoveeScreenHome;
    default:
        return app->notice_return;
    }
}

/* ------------------------------------------------------------------------- */
/* Scan and lightshow (widget)                                               */
/* ------------------------------------------------------------------------- */

static void on_retry(GuiButtonType button, InputType type, void* context) {
    if(button == GuiButtonTypeCenter && type == InputTypeShort) {
        post_ui(context, GoveeUiRetryScan, 0);
    }
}

static void scan_build(GoveeH6006App* app) {
    widget_reset(app->widget);
    char* t = app->widget_text;
    size_t ts = sizeof(app->widget_text);
    switch(app->scan_state) {
    case GoveeScanWaiting:
        snprintf(t, ts, "Finishing lamp updates\nbefore the scan...");
        break;
    case GoveeScanRunning:
    case GoveeScanStopping:
        snprintf(
            t,
            ts,
            "Searching for lamps\nFound %u, new %u\nSaved %u\nBack: stop",
            app->scan_seen_count,
            app->scan_new_count,
            app->registry->count);
        break;
    default:
        snprintf(
            t,
            ts,
            "%s\nBack: %s",
            app->scan_refused ? "Bluetooth scan refused." : "No lamps found.\nPlug the bulb in nearby.",
            app->registry->count ? "home" : "exit");
        widget_add_button_element(app->widget, GuiButtonTypeCenter, "Retry", on_retry, app);
        break;
    }
    widget_add_string_multiline_element(app->widget, 64, 4, AlignCenter, AlignTop, FontSecondary, t);
}

static void lightshow_build(GoveeH6006App* app) {
    widget_reset(app->widget);
    const char* name = "";
    uint8_t n = app->registry->count;
    if(app->target >= 1 && app->target <= n) {
        name = short_name(app->registry->lamps[app->target - 1u].name);
    } else if(n == 1) {
        name = short_name(app->registry->lamps[0].name);
    }
    snprintf(
        app->widget_text,
        sizeof(app->widget_text),
        "Lightshow: %s\n%s\nBack: stop",
        show_palette[app->show_sent % SHOW_COUNT].name, /* the colour just sent */
        name);
    widget_add_string_multiline_element(
        app->widget, 64, 8, AlignCenter, AlignTop, FontSecondary, app->widget_text);
}

/* ------------------------------------------------------------------------- */
/* Public                                                                     */
/* ------------------------------------------------------------------------- */

void govee_ui_alloc(GoveeH6006App* app) {
    ViewDispatcher* vd = app->view_dispatcher;

    app->home = variable_item_list_alloc();
    VariableItemList* h = app->home;
    app->row[GoveeHomePower] = variable_item_list_add(h, "Power", 2, on_power, app);
    app->row[GoveeHomeBrightness] =
        variable_item_list_add(h, "Brightness", BRI_STEPS, on_brightness, app);
    app->row[GoveeHomeMode] = variable_item_list_add(h, "Mode", 2, on_mode, app);
    app->row[GoveeHomeKelvin] = variable_item_list_add(h, "White temp", KELVIN_STEPS, on_kelvin, app);
    app->row[GoveeHomeColour] = variable_item_list_add(h, "Colour", 1, on_static, app);
    app->row[GoveeHomeLightshow] = variable_item_list_add(h, "Lightshow", 2, on_lightshow, app);
    app->row[GoveeHomeLamps] = variable_item_list_add(h, "Lamps", 1, on_static, app);
    app->row[GoveeHomeFind] = variable_item_list_add(h, "Find lamps", 1, on_static, app);
    app->row[GoveeHomeResult] = variable_item_list_add(h, "Result", 1, on_static, app);
    variable_item_list_set_enter_callback(h, on_home_enter, app);
    view_dispatcher_add_view(vd, GoveeViewHome, variable_item_list_get_view(h));

    app->lamps = submenu_alloc();
    view_dispatcher_add_view(vd, GoveeViewLamps, submenu_get_view(app->lamps));

    app->widget = widget_alloc();
    view_dispatcher_add_view(vd, GoveeViewWidget, widget_get_view(app->widget));

    app->dialog = dialog_ex_alloc();
    view_dispatcher_add_view(vd, GoveeViewDialog, dialog_ex_get_view(app->dialog));

    app->rgb_view = govee_view_rgb_picker_alloc(app);
    view_dispatcher_add_view(vd, GoveeViewRgbPicker, app->rgb_view);

    home_update(app);
}

void govee_ui_free(GoveeH6006App* app) {
    ViewDispatcher* vd = app->view_dispatcher;
    view_dispatcher_remove_view(vd, GoveeViewRgbPicker);
    govee_view_rgb_picker_free(app->rgb_view);
    view_dispatcher_remove_view(vd, GoveeViewDialog);
    dialog_ex_free(app->dialog);
    view_dispatcher_remove_view(vd, GoveeViewWidget);
    widget_free(app->widget);
    view_dispatcher_remove_view(vd, GoveeViewLamps);
    submenu_free(app->lamps);
    view_dispatcher_remove_view(vd, GoveeViewHome);
    variable_item_list_free(app->home);
}

void govee_ui_show(GoveeH6006App* app, GoveeScreen screen) {
    if(app->exiting) return;
    if(screen == GoveeScreenHome && app->notice_pending) {
        app->notice_pending = false;
        app->notice_return = GoveeScreenHome;
        screen = GoveeScreenNotice;
    }
    app->screen = screen;
    switch(screen) {
    case GoveeScreenScan:
        scan_build(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewWidget);
        break;
    case GoveeScreenHome:
        home_update(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewHome);
        break;
    case GoveeScreenLamps:
        lamps_build(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewLamps);
        break;
    case GoveeScreenLamp:
        lamp_build(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewDialog);
        break;
    case GoveeScreenColour:
        view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewRgbPicker);
        break;
    case GoveeScreenLightshow:
        lightshow_build(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewWidget);
        break;
    case GoveeScreenNotice:
        notice_build(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewDialog);
        break;
    default:
        break;
    }
}

void govee_ui_refresh(GoveeH6006App* app) {
    app->ui_dirty = false;
    if(app->exiting || !app->home) return;
    home_update(app);
    switch(app->screen) {
    case GoveeScreenScan:
        scan_build(app);
        break;
    case GoveeScreenLamps:
        lamps_build(app);
        break;
    case GoveeScreenLamp:
        if(app->lamp_sel < app->registry->count) {
            lamp_build(app);
        } else {
            govee_ui_show(app, GoveeScreenLamps);
        }
        break;
    case GoveeScreenLightshow:
        lightshow_build(app);
        break;
    default:
        break;
    }
}

void govee_ui_notice(GoveeH6006App* app, const char* text) {
    snprintf(app->notice, sizeof(app->notice), "%s", text);
    if(app->exiting) return;
    if(app->screen == GoveeScreenNone || app->screen == GoveeScreenScan ||
       app->screen == GoveeScreenNotice) {
        if(app->screen == GoveeScreenNotice) {
            notice_build(app); /* replace the text of the open notice */
        } else {
            app->notice_pending = true; /* shown when Home opens */
        }
        return;
    }
    app->notice_return = app->screen;
    govee_ui_show(app, GoveeScreenNotice);
}

void govee_ui_set_target(GoveeH6006App* app, uint8_t target) {
    if(target > app->registry->count) return;
    app->target = target;
    FURI_LOG_I(GOVEE_TAG, "target %u", target);
    if(app->lightshow && targeted_count(app) != 1) govee_ui_lightshow(app, false);
    govee_link_sync_engine(app);
    app->ui_dirty = true;
}

void govee_ui_forget(GoveeH6006App* app, uint8_t index0) {
    if(index0 >= app->registry->count) return;
    char addr[18];
    govee_registry_format_addr(app->registry->lamps[index0].id.addr, addr);
    govee_registry_remove(app->registry, index0);
    FURI_LOG_I(GOVEE_TAG, "lamp %u removed (%s)", index0 + 1u, addr);
    if(app->target == index0 + 1u) {
        app->target = 0;
    } else if(app->target > index0 + 1u) {
        app->target--;
    }
    app->registry_full = false;
    govee_files_save(app);
    if(app->lightshow && targeted_count(app) != 1) govee_ui_lightshow(app, false);
    govee_link_sync_engine(app);
    app->ui_dirty = true;
}

bool govee_ui_lightshow(GoveeH6006App* app, bool on) {
    if(on) {
        if(targeted_count(app) != 1) {
            FURI_LOG_W(GOVEE_TAG, "lightshow refused: %u lamps targeted", targeted_count(app));
            app->lightshow = false;
            app->ui_dirty = true;
            govee_ui_notice(app, "Lightshow needs exactly one lamp. Pick one in Lamps.");
            return false;
        }
        if(!app->lightshow) FURI_LOG_I(GOVEE_TAG, "lightshow start");
        app->lightshow = true;
        app->show_phase = 0;
        app->show_sent = 0;
        app->show_next_ms = furi_get_tick();
        govee_ui_show(app, GoveeScreenLightshow);
    } else {
        if(app->lightshow) FURI_LOG_I(GOVEE_TAG, "lightshow stop");
        app->lightshow = false;
        if(app->screen == GoveeScreenLightshow) govee_ui_show(app, GoveeScreenHome);
    }
    app->ui_dirty = true;
    return true;
}

void govee_ui_tick(GoveeH6006App* app) {
    if(!app->lightshow || app->exiting) return;
    uint32_t now = furi_get_tick();
    if(!govee_time_reached(now, app->show_next_ms)) return;
    if(targeted_count(app) != 1) {
        govee_ui_lightshow(app, false);
        return;
    }
    app->show_next_ms = now + GOVEE_LIGHTSHOW_STEP_MS;
    const ShowColour* c = &show_palette[app->show_phase % SHOW_COUNT];
    app->desired.power = true;
    app->desired.mode = GoveeModeColour;
    app->desired.rgb = c->rgb;
    govee_link_set_desired(app);
    app->show_sent = app->show_phase;
    app->show_phase = (uint8_t)((app->show_phase + 1u) % SHOW_COUNT);
    app->ui_dirty = true;
    govee_ui_refresh(app);
}

void govee_ui_handle_msg(GoveeH6006App* app, uint32_t action, uint32_t arg) {
    switch(action) {
    case GoveeUiRetryScan:
        if(app->screen == GoveeScreenScan && app->scan_state == GoveeScanNone) {
            app->scan_state = GoveeScanIdle;
            govee_link_request_scan(app, app->scan_initial);
        }
        break;
    case GoveeUiLampsSelect:
        if(app->screen != GoveeScreenLamps) break;
        if(arg == 0) {
            govee_ui_set_target(app, 0);
            govee_ui_refresh(app);
        } else if(arg <= app->registry->count) {
            app->lamp_sel = (uint8_t)(arg - 1u);
            govee_ui_show(app, GoveeScreenLamp);
        }
        break;
    case GoveeUiLampDialog:
        if(app->screen == GoveeScreenNotice) {
            govee_ui_show(app, notice_back_screen(app));
            break;
        }
        if(app->screen != GoveeScreenLamp || app->lamp_sel >= app->registry->count) break;
        if(arg == DialogExResultLeft) {
            govee_ui_forget(app, app->lamp_sel);
            /* a failed save opened a notice; it returns to Lamps itself */
            if(app->screen == GoveeScreenLamp) {
                govee_ui_show(app, app->registry->count ? GoveeScreenLamps : GoveeScreenHome);
            }
        } else if(arg == DialogExResultCenter) {
            govee_ui_set_target(app, 0);
            govee_ui_show(app, GoveeScreenLamps);
        } else if(arg == DialogExResultRight) {
            govee_ui_set_target(app, (uint8_t)(app->lamp_sel + 1u));
            govee_ui_show(app, GoveeScreenLamps);
        }
        break;
    default:
        break;
    }
    if(app->ui_dirty) govee_ui_refresh(app);
}

/* Back on each screen. @return false to exit the app */
bool govee_ui_back(GoveeH6006App* app) {
    switch(app->screen) {
    case GoveeScreenScan:
        if(app->scan_state == GoveeScanRunning || app->scan_state == GoveeScanWaiting) {
            govee_link_abort_scan(app);
        }
        if(app->registry->count == 0) return false;
        if(app->scan_state == GoveeScanNone) app->scan_state = GoveeScanIdle;
        govee_ui_show(app, GoveeScreenHome);
        return true;
    case GoveeScreenHome:
        return false;
    case GoveeScreenLamps:
    case GoveeScreenColour:
        govee_ui_show(app, GoveeScreenHome);
        return true;
    case GoveeScreenLamp:
        govee_ui_show(app, GoveeScreenLamps);
        return true;
    case GoveeScreenLightshow:
        govee_ui_lightshow(app, false);
        govee_ui_show(app, GoveeScreenHome);
        return true;
    case GoveeScreenNotice:
        govee_ui_show(app, notice_back_screen(app));
        return true;
    default:
        return false;
    }
}

const char* govee_ui_state_token(const GoveeH6006App* app) {
    if(app->exiting) return "exiting";
    switch(app->screen) {
    case GoveeScreenScan:
        return app->scan_state == GoveeScanNone ? "scan_none" : "scan";
    case GoveeScreenHome:
        return "home";
    case GoveeScreenLamps:
        return "lamps";
    case GoveeScreenLamp:
        return "lamp";
    case GoveeScreenColour:
        return "colour";
    case GoveeScreenLightshow:
        return "lightshow";
    case GoveeScreenNotice:
        return "notice";
    default:
        return "start";
    }
}

/* Colour picker (views/view_rgb_picker.c), app thread inside its input callback. */
void govee_app_on_colour(GoveeH6006App* app, uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
    if(app->lightshow) govee_ui_lightshow(app, false);
    app->colour_index = index;
    app->desired.power = true;
    app->desired.mode = GoveeModeColour;
    app->desired.rgb = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    govee_link_set_desired(app);
    govee_ui_refresh(app);
}

void govee_app_on_colour_back(GoveeH6006App* app) {
    govee_ui_show(app, GoveeScreenHome);
}
