#pragma once

#include <gui/view.h>

// Opaque app context pointer - include govee_h6006_app.h for the full type
View* govee_view_rgb_picker_alloc(void* app_ctx);
void govee_view_rgb_picker_free(View* view);
