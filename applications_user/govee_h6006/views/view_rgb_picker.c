#include "../govee_h6006_app.h"
#include "view_rgb_picker.h"

#define TAG "GoveeViewRgbPicker"

// Simplified 6-color palette grid (2 rows x 3 columns)
// Monochrome display: render as labeled color names with cursor highlight

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    const char* name;
} RgbColor;

static const RgbColor rgb_palette[6] = {
    {255, 0,   0,   "Red"},
    {0,   255, 0,   "Green"},
    {0,   0,   255, "Blue"},
    {255, 255, 0,   "Yellow"},
    {0,   255, 255, "Cyan"},
    {255, 0,   255, "Magenta"},
};

typedef struct {
    uint8_t cursor; // 0..5
    GoveeH6006App* app;
} RgbPickerModel;

static void rgb_picker_draw_callback(Canvas* canvas, void* model) {
    RgbPickerModel* m = model;

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, "RGB Picker");

    canvas_set_font(canvas, FontSecondary);

    // Draw 2 rows x 3 columns
    for(uint8_t i = 0; i < 6; i++) {
        uint8_t col = i % 3;
        uint8_t row = i / 3;
        int16_t x = (int16_t)(8 + col * 38);
        int16_t y = (int16_t)(18 + row * 20);

        if(m->cursor == i) {
            // Draw selection box
            canvas_draw_rbox(canvas, x - 2, y - 1, 36, 14, 2);
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_str(canvas, (int16_t)(x + 1), (int16_t)(y + 9), rgb_palette[i].name);
            canvas_set_color(canvas, ColorBlack);
        } else {
            canvas_draw_rframe(canvas, x - 2, y - 1, 36, 14, 2);
            canvas_draw_str(canvas, (int16_t)(x + 1), (int16_t)(y + 9), rgb_palette[i].name);
        }
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 60, AlignCenter, AlignBottom, "OK=send  Back=exit");
}

static bool rgb_picker_input_callback(InputEvent* event, void* context) {
    furi_assert(context);
    View* view = context;
    RgbPickerModel* m = view_get_model(view);
    bool consumed = false;

    if(event->type == InputTypeShort || event->type == InputTypeRepeat) {
        switch(event->key) {
        case InputKeyLeft:
            if(m->cursor > 0) m->cursor--;
            consumed = true;
            break;
        case InputKeyRight:
            if(m->cursor < 5) m->cursor++;
            consumed = true;
            break;
        case InputKeyUp:
            if(m->cursor >= 3) m->cursor -= 3;
            consumed = true;
            break;
        case InputKeyDown:
            if(m->cursor + 3 < 6) m->cursor += 3;
            consumed = true;
            break;
        case InputKeyOk: {
            GoveeH6006App* app = m->app;
            const RgbColor* c = &rgb_palette[m->cursor];
            app->rgb[0] = c->r;
            app->rgb[1] = c->g;
            app->rgb[2] = c->b;
            govee_central_send_rgb(app->central, c->r, c->g, c->b);
            consumed = true;
            break;
        }
        case InputKeyBack:
            // Return to control view - switch back
            {
                GoveeH6006App* app = m->app;
                view_dispatcher_switch_to_view(
                    app->view_dispatcher, GoveeViewVariableItemList);
                consumed = true;
            }
            break;
        default:
            break;
        }
    }

    view_commit_model(view, consumed);
    return consumed;
}

View* govee_view_rgb_picker_alloc(void* app_ctx) {
    GoveeH6006App* app = app_ctx;
    View* view = view_alloc();
    view_set_draw_callback(view, rgb_picker_draw_callback);
    view_set_input_callback(view, rgb_picker_input_callback);
    view_set_context(view, view);

    view_allocate_model(view, ViewModelTypeLockFree, sizeof(RgbPickerModel));
    RgbPickerModel* m = view_get_model(view);
    m->cursor = 0;
    m->app = app;
    view_commit_model(view, false);

    return view;
}

void govee_view_rgb_picker_free(View* view) {
    furi_assert(view);
    view_free(view);
}
