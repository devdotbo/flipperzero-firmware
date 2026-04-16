#include "../govee_h6006_app.h"
#include "scenes.h"
#include <gui/elements.h>

#define TAG "GoveeSceneLightshow"

#define LIGHTSHOW_PHASE_COUNT 8
#define LIGHTSHOW_INTERVAL_MS 500  // 2 Hz

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    const char* name;
} LightshowColor;

static const LightshowColor lightshow_palette[LIGHTSHOW_PHASE_COUNT] = {
    {255, 0,   0,   "Red"},
    {255, 128, 0,   "Orange"},
    {255, 255, 0,   "Yellow"},
    {0,   255, 0,   "Green"},
    {0,   255, 255, "Cyan"},
    {0,   0,   255, "Blue"},
    {180, 0,   255, "Violet"},
    {255, 0,   180, "Pink"},
};

static void lightshow_timer_callback(void* context) {
    GoveeH6006App* app = context;
    furi_assert(app);

    const LightshowColor* c = &lightshow_palette[app->lightshow_phase];
    govee_central_send_rgb(app->central, c->r, c->g, c->b);

    app->lightshow_phase = (app->lightshow_phase + 1) % LIGHTSHOW_PHASE_COUNT;

    // Update widget display
    widget_reset(app->widget);

    char header[32];
    snprintf(header, sizeof(header), "Lightshow: %s", lightshow_palette[app->lightshow_phase].name);
    widget_add_string_element(app->widget, 64, 4, AlignCenter, AlignTop, FontPrimary, header);

    // Simple progress bar showing phase
    uint8_t bar_w = (uint8_t)(((app->lightshow_phase + 1) * 100) / LIGHTSHOW_PHASE_COUNT);
    widget_add_string_element(
        app->widget, 64, 28, AlignCenter, AlignCenter, FontSecondary, "Press BACK to stop");

    // Draw a manual progress indicator as text
    char prog[12];
    snprintf(
        prog, sizeof(prog), "[%u/%u]", app->lightshow_phase + 1, LIGHTSHOW_PHASE_COUNT);
    widget_add_string_element(app->widget, 64, 44, AlignCenter, AlignBottom, FontSecondary, prog);

    UNUSED(bar_w);
}

void govee_scene_lightshow_on_enter(void* ctx) {
    GoveeH6006App* app = ctx;
    furi_assert(app);

    app->lightshow_phase = 0;

    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 4, AlignCenter, AlignTop, FontPrimary, "Lightshow");
    widget_add_string_element(
        app->widget, 64, 28, AlignCenter, AlignCenter, FontSecondary, "Press BACK to stop");

    if(!app->lightshow_timer) {
        app->lightshow_timer =
            furi_timer_alloc(lightshow_timer_callback, FuriTimerTypePeriodic, app);
    }
    furi_timer_start(app->lightshow_timer, LIGHTSHOW_INTERVAL_MS);

    view_dispatcher_switch_to_view(app->view_dispatcher, GoveeViewWidget);
}

bool govee_scene_lightshow_on_event(void* ctx, SceneManagerEvent event) {
    UNUSED(ctx);
    UNUSED(event);
    return false;
}

void govee_scene_lightshow_on_exit(void* ctx) {
    GoveeH6006App* app = ctx;
    if(app->lightshow_timer) {
        furi_timer_stop(app->lightshow_timer);
        furi_timer_free(app->lightshow_timer);
        app->lightshow_timer = NULL;
    }
    widget_reset(app->widget);
}
