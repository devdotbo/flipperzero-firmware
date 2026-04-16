#pragma once

#include "govee_central.h"
#include "../storage/bulb_cache.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct GoveeGroup GoveeGroup;

typedef struct {
    bool set_power;
    bool power;
    bool set_brightness;
    uint8_t brightness;
    bool set_ct;
    uint16_t kelvin;
    bool set_rgb;
    uint8_t r;
    uint8_t g;
    uint8_t b;
} GoveeGroupCommandSet;

typedef enum {
    GoveeGroupEventProgress,   // data = const GoveeGroupProgress*
    GoveeGroupEventBulbDone,   // data = const GoveeGroupProgress*
    GoveeGroupEventBulbFailed, // data = const GoveeGroupProgress*
    GoveeGroupEventAllDone,    // data = NULL; totals queried via getters
    GoveeGroupEventAborted,    // data = NULL
} GoveeGroupEvent;

typedef struct {
    uint32_t idx;      // 0-based index of bulb being visited
    uint32_t total;    // total bulbs in this run
    uint8_t addr[6];
    char name[32];
} GoveeGroupProgress;

typedef void (*GoveeGroupCallback)(
    GoveeGroup* grp,
    GoveeGroupEvent event,
    const void* data,
    void* ctx);

GoveeGroup* govee_group_alloc(GoveeCentral* central, GoveeBulbCache* cache);
void govee_group_free(GoveeGroup* grp);

void govee_group_set_callback(GoveeGroup* grp, GoveeGroupCallback cb, void* ctx);

// Start a fan-out run. Copies cache entries into an internal list; safe to
// modify cache after this returns. If skip_addr is non-NULL, entries with
// matching address are excluded.
// Returns false if already running or the resulting target list is empty.
bool govee_group_apply_all(
    GoveeGroup* grp,
    const GoveeGroupCommandSet* cmds,
    const uint8_t* skip_addr);

void govee_group_abort(GoveeGroup* grp);
bool govee_group_is_running(GoveeGroup* grp);

uint32_t govee_group_total(GoveeGroup* grp);
uint32_t govee_group_done_count(GoveeGroup* grp);
uint32_t govee_group_failed_count(GoveeGroup* grp);
