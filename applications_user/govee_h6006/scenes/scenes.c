#include "scenes.h"
#include "../govee_h6006_app.h"

// Generate on_enter handlers array
#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_enter,
void (*const govee_scene_on_enter_handlers[])(void*) = {
#include "scenes_config.h"
};
#undef ADD_SCENE

// Generate on_event handlers array
#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_event,
bool (*const govee_scene_on_event_handlers[])(void* context, SceneManagerEvent event) = {
#include "scenes_config.h"
};
#undef ADD_SCENE

// Generate on_exit handlers array
#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_exit,
void (*const govee_scene_on_exit_handlers[])(void* context) = {
#include "scenes_config.h"
};
#undef ADD_SCENE

const SceneManagerHandlers govee_scene_handlers = {
    .on_enter_handlers = govee_scene_on_enter_handlers,
    .on_event_handlers = govee_scene_on_event_handlers,
    .on_exit_handlers = govee_scene_on_exit_handlers,
    .scene_num = GoveeSceneCount,
};
