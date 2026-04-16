#include "govee_group.h"

#include <furi.h>
#include <string.h>
#include <stdlib.h>

#define TAG "GoveeGroup"

#define GROUP_STEP_TIMEOUT_MS 5000
#define GROUP_MAX_TARGETS     GOVEE_CACHE_MAX_BULBS

typedef enum {
    GroupStateIdle,
    GroupStateWaitingInitialDisconnect,
    GroupStateConnecting,
    GroupStateWriting,
    GroupStateDisconnecting,
    GroupStateFinalizing,
} GroupState;

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    char name[GOVEE_CACHE_NAME_LEN];
} GroupTarget;

// Sequence of writes applied per bulb after discovery completes.
// Matches the field order in GoveeGroupCommandSet.
typedef enum {
    WriteStepPower,
    WriteStepBrightness,
    WriteStepCt,
    WriteStepRgb,
    WriteStepCount,
} WriteStep;

struct GoveeGroup {
    GoveeCentral* central;
    GoveeBulbCache* cache;

    // Caller's callback + ctx
    GoveeGroupCallback user_cb;
    void* user_ctx;

    // Saved central callback (installed before apply_all, restored after)
    GoveeCentralEventCallback saved_central_cb;
    void* saved_central_ctx;

    FuriTimer* step_timer;

    GroupTarget targets[GROUP_MAX_TARGETS];
    uint32_t target_count;
    uint32_t target_idx;

    GoveeGroupCommandSet cmds;

    GroupState state;
    WriteStep write_step;
    bool current_bulb_failed; // true once any step in current target failed

    uint32_t done_count;
    uint32_t failed_count;
};

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

static void group_central_callback(
    GoveeCentral* gc,
    GoveeCentralEvent event,
    const void* data,
    void* context);
static void group_step_timer_cb(void* ctx);
static void advance_to_next_target(GoveeGroup* grp);
static void start_current_target(GoveeGroup* grp);
static void finish_current_target(GoveeGroup* grp, bool failed);
static bool issue_next_write(GoveeGroup* grp);
static void emit_progress(GoveeGroup* grp, GoveeGroupEvent ev);
static void restore_and_finalize(GoveeGroup* grp, GoveeGroupEvent terminal);

// ---------------------------------------------------------------------------
// Alloc / free
// ---------------------------------------------------------------------------

GoveeGroup* govee_group_alloc(GoveeCentral* central, GoveeBulbCache* cache) {
    furi_check(central != NULL);
    furi_check(cache != NULL);

    GoveeGroup* grp = malloc(sizeof(GoveeGroup));
    furi_check(grp != NULL);
    memset(grp, 0, sizeof(GoveeGroup));

    grp->central = central;
    grp->cache = cache;
    grp->state = GroupStateIdle;
    grp->step_timer = furi_timer_alloc(group_step_timer_cb, FuriTimerTypeOnce, grp);
    furi_check(grp->step_timer != NULL);

    return grp;
}

void govee_group_free(GoveeGroup* grp) {
    furi_check(grp != NULL);
    if(grp->state != GroupStateIdle) {
        govee_group_abort(grp);
    }
    furi_timer_free(grp->step_timer);
    free(grp);
}

void govee_group_set_callback(GoveeGroup* grp, GoveeGroupCallback cb, void* ctx) {
    furi_check(grp != NULL);
    grp->user_cb = cb;
    grp->user_ctx = ctx;
}

bool govee_group_is_running(GoveeGroup* grp) {
    furi_check(grp != NULL);
    return grp->state != GroupStateIdle;
}

uint32_t govee_group_total(GoveeGroup* grp) {
    furi_check(grp != NULL);
    return grp->target_count;
}

uint32_t govee_group_done_count(GoveeGroup* grp) {
    furi_check(grp != NULL);
    return grp->done_count;
}

uint32_t govee_group_failed_count(GoveeGroup* grp) {
    furi_check(grp != NULL);
    return grp->failed_count;
}

// ---------------------------------------------------------------------------
// Run
// ---------------------------------------------------------------------------

bool govee_group_apply_all(
    GoveeGroup* grp,
    const GoveeGroupCommandSet* cmds,
    const uint8_t* skip_addr) {
    furi_check(grp != NULL);
    furi_check(cmds != NULL);

    if(grp->state != GroupStateIdle) {
        FURI_LOG_W(TAG, "apply_all called while running");
        return false;
    }

    // Snapshot cache into targets, optionally skipping one address.
    grp->target_count = 0;
    size_t cache_n = govee_bulb_cache_count(grp->cache);
    for(size_t i = 0; i < cache_n && grp->target_count < GROUP_MAX_TARGETS; i++) {
        const GoveeBulbCacheEntry* e = govee_bulb_cache_get(grp->cache, i);
        if(!e) continue;
        if(skip_addr && memcmp(e->addr, skip_addr, 6) == 0) continue;

        GroupTarget* t = &grp->targets[grp->target_count++];
        memcpy(t->addr, e->addr, 6);
        t->addr_type = e->addr_type;
        strlcpy(t->name, e->name, sizeof(t->name));
    }

    if(grp->target_count == 0) {
        FURI_LOG_W(TAG, "apply_all: no targets after skip filter");
        return false;
    }

    grp->cmds = *cmds;
    grp->target_idx = 0;
    grp->done_count = 0;
    grp->failed_count = 0;
    grp->current_bulb_failed = false;

    // Install our callback on central, saving the app's original.
    grp->saved_central_cb = NULL;
    grp->saved_central_ctx = NULL;
    // We don't have a getter for the existing callback; we just overwrite and
    // the scene restores the app callback via set_callback(app_cb, app) after
    // the terminal event. That keeps this module's state simpler.
    govee_central_set_callback(grp->central, group_central_callback, grp);

    FURI_LOG_I(TAG, "apply_all starting, %lu targets", (unsigned long)grp->target_count);

    if(govee_central_is_connected(grp->central)) {
        // Central still holds a prior link. Disconnect and wait for the event
        // before launching the first target; async Disconnected arrival drives
        // the transition in group_central_callback.
        grp->state = GroupStateWaitingInitialDisconnect;
        govee_central_disconnect(grp->central);
        furi_timer_start(grp->step_timer, GROUP_STEP_TIMEOUT_MS);
    } else {
        start_current_target(grp);
    }
    return true;
}

void govee_group_abort(GoveeGroup* grp) {
    furi_check(grp != NULL);
    if(grp->state == GroupStateIdle) return;

    FURI_LOG_I(TAG, "abort");
    furi_timer_stop(grp->step_timer);
    if(govee_central_is_connected(grp->central)) {
        govee_central_disconnect(grp->central);
    }
    restore_and_finalize(grp, GoveeGroupEventAborted);
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

static void start_current_target(GoveeGroup* grp) {
    if(grp->target_idx >= grp->target_count) {
        restore_and_finalize(grp, GoveeGroupEventAllDone);
        return;
    }

    GroupTarget* t = &grp->targets[grp->target_idx];
    grp->state = GroupStateConnecting;
    grp->write_step = WriteStepPower;
    grp->current_bulb_failed = false;

    FURI_LOG_I(
        TAG,
        "target %lu/%lu: %s",
        (unsigned long)(grp->target_idx + 1),
        (unsigned long)grp->target_count,
        t->name);

    emit_progress(grp, GoveeGroupEventProgress);

    // Central is expected to be disconnected at this point (either we were
    // disconnected at apply_all time, or a prior target's finish sequence
    // drove us through Disconnecting -> Disconnected).
    if(!govee_central_connect(grp->central, t->addr, t->addr_type)) {
        FURI_LOG_E(TAG, "connect failed on target %lu", (unsigned long)grp->target_idx);
        finish_current_target(grp, true);
        return;
    }
    furi_timer_start(grp->step_timer, GROUP_STEP_TIMEOUT_MS);
}

static void finish_current_target(GoveeGroup* grp, bool failed) {
    furi_timer_stop(grp->step_timer);

    if(failed) {
        grp->failed_count++;
        emit_progress(grp, GoveeGroupEventBulbFailed);
    } else {
        grp->done_count++;
        emit_progress(grp, GoveeGroupEventBulbDone);
    }

    if(govee_central_is_connected(grp->central)) {
        grp->state = GroupStateDisconnecting;
        govee_central_disconnect(grp->central);
        // Disconnected event will drive advance_to_next_target
        furi_timer_start(grp->step_timer, GROUP_STEP_TIMEOUT_MS);
    } else {
        advance_to_next_target(grp);
    }
}

static void advance_to_next_target(GoveeGroup* grp) {
    grp->target_idx++;
    start_current_target(grp);
}

static bool issue_next_write(GoveeGroup* grp) {
    while(grp->write_step < WriteStepCount) {
        bool sent = false;
        switch(grp->write_step) {
        case WriteStepPower:
            if(grp->cmds.set_power) {
                sent = govee_central_send_power(grp->central, grp->cmds.power);
            }
            break;
        case WriteStepBrightness:
            if(grp->cmds.set_brightness) {
                sent = govee_central_send_brightness(grp->central, grp->cmds.brightness);
            }
            break;
        case WriteStepCt:
            if(grp->cmds.set_ct) {
                sent = govee_central_send_ct(grp->central, grp->cmds.kelvin);
            }
            break;
        case WriteStepRgb:
            if(grp->cmds.set_rgb) {
                sent = govee_central_send_rgb(
                    grp->central, grp->cmds.r, grp->cmds.g, grp->cmds.b);
            }
            break;
        default:
            break;
        }

        if(sent) {
            furi_timer_start(grp->step_timer, GROUP_STEP_TIMEOUT_MS);
            return true;
        }

        // Flag not set or send rejected; advance to next write step.
        grp->write_step++;
    }
    // No more writes — target done.
    return false;
}

static void emit_progress(GoveeGroup* grp, GoveeGroupEvent ev) {
    if(!grp->user_cb) return;
    if(grp->target_idx >= grp->target_count) return;

    GroupTarget* t = &grp->targets[grp->target_idx];
    GoveeGroupProgress p = {0};
    p.idx = grp->target_idx;
    p.total = grp->target_count;
    memcpy(p.addr, t->addr, 6);
    strlcpy(p.name, t->name, sizeof(p.name));

    grp->user_cb(grp, ev, &p, grp->user_ctx);
}

static void restore_and_finalize(GoveeGroup* grp, GoveeGroupEvent terminal) {
    furi_timer_stop(grp->step_timer);
    grp->state = GroupStateFinalizing;

    // Caller (scene) is responsible for re-installing the app's own callback
    // and reconnecting to any prior bulb. We just emit the terminal event.
    GoveeGroupCallback cb = grp->user_cb;
    void* ctx = grp->user_ctx;

    grp->state = GroupStateIdle;

    if(cb) {
        cb(grp, terminal, NULL, ctx);
    }
}

// ---------------------------------------------------------------------------
// Central event handler (runs only while group owns the callback)
// ---------------------------------------------------------------------------

static void group_central_callback(
    GoveeCentral* gc,
    GoveeCentralEvent event,
    const void* data,
    void* context) {
    UNUSED(gc);
    UNUSED(data);
    GoveeGroup* grp = context;
    if(!grp || grp->state == GroupStateIdle) return;

    switch(event) {
    case GoveeCentralEventScanResult:
    case GoveeCentralEventScanComplete:
        // Stale scan events during group run: ignore.
        break;

    case GoveeCentralEventConnected:
    case GoveeCentralEventDiscoveryComplete:
        // govee_central.c emits Connected *after* discovery completes and
        // the write characteristic is resolved. Both map to "ready to write."
        if(grp->state == GroupStateConnecting) {
            grp->state = GroupStateWriting;
            if(!issue_next_write(grp)) {
                // No flags enabled in command set -> nothing to write, done.
                finish_current_target(grp, false);
            }
        }
        break;

    case GoveeCentralEventWriteComplete:
        if(grp->state == GroupStateWriting) {
            grp->write_step++;
            if(!issue_next_write(grp)) {
                finish_current_target(grp, grp->current_bulb_failed);
            }
        }
        break;

    case GoveeCentralEventDisconnected:
        if(grp->state == GroupStateWaitingInitialDisconnect) {
            furi_timer_stop(grp->step_timer);
            start_current_target(grp);
        } else if(grp->state == GroupStateDisconnecting) {
            advance_to_next_target(grp);
        } else if(
            grp->state == GroupStateConnecting || grp->state == GroupStateWriting) {
            // Unexpected drop mid-flight -> failure
            finish_current_target(grp, true);
        }
        break;

    case GoveeCentralEventError:
        grp->current_bulb_failed = true;
        if(grp->state == GroupStateConnecting || grp->state == GroupStateWriting) {
            finish_current_target(grp, true);
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// Per-step timeout
// ---------------------------------------------------------------------------

static void group_step_timer_cb(void* ctx) {
    GoveeGroup* grp = ctx;
    if(!grp || grp->state == GroupStateIdle) return;

    FURI_LOG_W(TAG, "step timeout in state %d", (int)grp->state);

    if(grp->state == GroupStateWaitingInitialDisconnect) {
        // The central never emitted Disconnected; assume it's gone anyway.
        start_current_target(grp);
    } else if(grp->state == GroupStateDisconnecting) {
        // Disconnect took too long; advance anyway
        advance_to_next_target(grp);
    } else {
        finish_current_target(grp, true);
    }
}
