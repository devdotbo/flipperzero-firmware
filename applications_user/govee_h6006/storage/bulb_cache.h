#pragma once

#include <furi.h>
#include <stdbool.h>
#include <stdint.h>

#define GOVEE_CACHE_MAX_BULBS 8
#define GOVEE_CACHE_NAME_LEN  32

typedef struct {
    char name[GOVEE_CACHE_NAME_LEN];
    uint8_t addr[6];
    uint8_t addr_type;
    uint32_t last_seen_ts;
} GoveeBulbCacheEntry;

typedef struct GoveeBulbCache GoveeBulbCache;

GoveeBulbCache* govee_bulb_cache_alloc(void);
void govee_bulb_cache_free(GoveeBulbCache* cache);

bool govee_bulb_cache_load(GoveeBulbCache* cache, const char* path);
bool govee_bulb_cache_save(GoveeBulbCache* cache, const char* path);

size_t govee_bulb_cache_count(GoveeBulbCache* cache);
const GoveeBulbCacheEntry* govee_bulb_cache_get(GoveeBulbCache* cache, size_t index);

bool govee_bulb_cache_upsert(
    GoveeBulbCache* cache,
    const char* name,
    const uint8_t addr[6],
    uint8_t addr_type);

bool govee_bulb_cache_remove(GoveeBulbCache* cache, size_t index);
void govee_bulb_cache_clear(GoveeBulbCache* cache);
