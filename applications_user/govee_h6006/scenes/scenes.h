#pragma once

#include <gui/scene_manager.h>

extern const SceneManagerHandlers govee_scene_handlers;

// Declare on_enter handlers
#define ADD_SCENE(prefix, name, id) void prefix##_scene_##name##_on_enter(void* ctx);
#include "scenes_config.h"
#undef ADD_SCENE

// Declare on_event handlers
#define ADD_SCENE(prefix, name, id) \
    bool prefix##_scene_##name##_on_event(void* ctx, SceneManagerEvent event);
#include "scenes_config.h"
#undef ADD_SCENE

// Declare on_exit handlers
#define ADD_SCENE(prefix, name, id) void prefix##_scene_##name##_on_exit(void* ctx);
#include "scenes_config.h"
#undef ADD_SCENE
