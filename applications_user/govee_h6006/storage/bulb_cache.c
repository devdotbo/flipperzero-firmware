#include "bulb_cache.h"

#include <furi.h>
#include <furi_hal_rtc.h>
#include <storage/storage.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#define TAG "BulbCache"
#define GOVEE_CACHE_DIR "/ext/apps_data/govee_h6006"

struct GoveeBulbCache {
    GoveeBulbCacheEntry entries[GOVEE_CACHE_MAX_BULBS];
    size_t count;
};

// ---------------------------------------------------------------------------
// alloc / free
// ---------------------------------------------------------------------------

GoveeBulbCache* govee_bulb_cache_alloc(void) {
    GoveeBulbCache* cache = malloc(sizeof(GoveeBulbCache));
    furi_check(cache != NULL);
    memset(cache, 0, sizeof(GoveeBulbCache));
    return cache;
}

void govee_bulb_cache_free(GoveeBulbCache* cache) {
    furi_check(cache != NULL);
    free(cache);
}

// ---------------------------------------------------------------------------
// count / get
// ---------------------------------------------------------------------------

size_t govee_bulb_cache_count(GoveeBulbCache* cache) {
    furi_check(cache != NULL);
    return cache->count;
}

const GoveeBulbCacheEntry* govee_bulb_cache_get(GoveeBulbCache* cache, size_t index) {
    furi_check(cache != NULL);
    if(index >= cache->count) return NULL;
    return &cache->entries[index];
}

// ---------------------------------------------------------------------------
// upsert
// ---------------------------------------------------------------------------

bool govee_bulb_cache_upsert(
    GoveeBulbCache* cache,
    const char* name,
    const uint8_t addr[6],
    uint8_t addr_type) {
    furi_check(cache != NULL);
    furi_check(name != NULL);
    furi_check(addr != NULL);

    uint32_t ts = furi_hal_rtc_get_timestamp();

    // search for existing entry by address
    for(size_t i = 0; i < cache->count; i++) {
        if(memcmp(cache->entries[i].addr, addr, 6) == 0) {
            strlcpy(cache->entries[i].name, name, GOVEE_CACHE_NAME_LEN);
            cache->entries[i].addr_type = addr_type;
            cache->entries[i].last_seen_ts = ts;
            return true;
        }
    }

    if(cache->count >= GOVEE_CACHE_MAX_BULBS) return false;

    GoveeBulbCacheEntry* e = &cache->entries[cache->count];
    strlcpy(e->name, name, GOVEE_CACHE_NAME_LEN);
    memcpy(e->addr, addr, 6);
    e->addr_type = addr_type;
    e->last_seen_ts = ts;
    cache->count++;
    return true;
}

// ---------------------------------------------------------------------------
// remove / clear
// ---------------------------------------------------------------------------

bool govee_bulb_cache_remove(GoveeBulbCache* cache, size_t index) {
    furi_check(cache != NULL);
    if(index >= cache->count) return false;
    // shift entries left
    for(size_t i = index; i + 1 < cache->count; i++) {
        cache->entries[i] = cache->entries[i + 1];
    }
    cache->count--;
    memset(&cache->entries[cache->count], 0, sizeof(GoveeBulbCacheEntry));
    return true;
}

void govee_bulb_cache_clear(GoveeBulbCache* cache) {
    furi_check(cache != NULL);
    memset(cache->entries, 0, sizeof(cache->entries));
    cache->count = 0;
}

// ---------------------------------------------------------------------------
// Minimal JSON helpers
// ---------------------------------------------------------------------------

// Advance past whitespace
static const char* skip_ws(const char* p) {
    while(*p && isspace((unsigned char)*p)) p++;
    return p;
}

// Find next occurrence of ch starting from p
static const char* find_char(const char* p, char ch) {
    while(*p && *p != ch) p++;
    return *p ? p : NULL;
}

// Copy quoted string starting just after opening quote into buf (max len incl NUL).
// Returns pointer past closing quote, or NULL on error.
static const char* read_quoted(const char* p, char* buf, size_t len) {
    // p points to opening '"'
    p++; // skip '"'
    size_t i = 0;
    while(*p && *p != '"') {
        if(i + 1 < len) buf[i++] = *p;
        p++;
    }
    if(*p != '"') return NULL;
    buf[i] = '\0';
    return p + 1; // past closing '"'
}

// Read a decimal uint32 from p; advance past digits. Returns pointer after digits.
static const char* read_uint32(const char* p, uint32_t* out) {
    *out = 0;
    if(!isdigit((unsigned char)*p)) return NULL;
    while(isdigit((unsigned char)*p)) {
        *out = (*out) * 10 + (uint32_t)(*p - '0');
        p++;
    }
    return p;
}

// ---------------------------------------------------------------------------
// load
// ---------------------------------------------------------------------------

bool govee_bulb_cache_load(GoveeBulbCache* cache, const char* path) {
    furi_check(cache != NULL);
    furi_check(path != NULL);

    govee_bulb_cache_clear(cache);

    Storage* storage = furi_record_open(RECORD_STORAGE);

    if(!storage_file_exists(storage, path)) {
        // no file yet — fresh cache, not an error
        furi_record_close(RECORD_STORAGE);
        return true;
    }

    File* file = storage_file_alloc(storage);
    if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        FURI_LOG_E(TAG, "Failed to open cache file for read: %s", path);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return false;
    }

    uint64_t file_size = storage_file_size(file);
    if(file_size == 0 || file_size > 8192) {
        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        // empty or suspiciously large — treat as empty
        return true;
    }

    char* buf = malloc(file_size + 1);
    if(!buf) {
        FURI_LOG_E(TAG, "OOM reading cache");
        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return false;
    }

    uint16_t rd = storage_file_read(file, buf, (uint16_t)file_size);
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    buf[rd] = '\0';

    // --- minimal parser ---
    // Walk through the buffer looking for object boundaries '{' ... '}'
    const char* p = buf;
    while(*p) {
        p = skip_ws(p);
        if(*p != '{') { p++; continue; }
        // We're inside an object — collect fields until '}'
        const char* obj_end = find_char(p, '}');
        if(!obj_end) break;

        char name[GOVEE_CACHE_NAME_LEN] = {0};
        char addr_str[18] = {0};
        uint32_t addr_type_u = 0;
        uint32_t last_seen_ts = 0;
        bool has_name = false, has_addr = false, has_type = false, has_ts = false;

        const char* q = p + 1; // inside '{'
        while(q < obj_end) {
            q = skip_ws(q);
            if(*q != '"') { q++; continue; }

            // read key
            char key[32] = {0};
            q = read_quoted(q, key, sizeof(key));
            if(!q) break;

            q = skip_ws(q);
            if(*q != ':') break;
            q++;
            q = skip_ws(q);

            if(strcmp(key, "name") == 0 && *q == '"') {
                q = read_quoted(q, name, sizeof(name));
                has_name = (q != NULL);
            } else if(strcmp(key, "address") == 0 && *q == '"') {
                q = read_quoted(q, addr_str, sizeof(addr_str));
                has_addr = (q != NULL);
            } else if(strcmp(key, "addr_type") == 0) {
                q = read_uint32(q, &addr_type_u);
                has_type = (q != NULL);
            } else if(strcmp(key, "last_seen_ts") == 0) {
                q = read_uint32(q, &last_seen_ts);
                has_ts = (q != NULL);
            } else {
                // skip value — either string or number
                if(*q == '"') {
                    char tmp[64];
                    q = read_quoted(q, tmp, sizeof(tmp));
                } else {
                    while(q && *q && *q != ',' && *q != '}') q++;
                }
            }

            if(!q) break;
            // skip optional comma/whitespace
            q = skip_ws(q);
            if(*q == ',') q++;
        }

        if(has_name && has_addr && has_type && has_ts && cache->count < GOVEE_CACHE_MAX_BULBS) {
            uint8_t a[6] = {0};
            int parsed = sscanf(
                addr_str,
                "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
                &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]);
            if(parsed == 6) {
                GoveeBulbCacheEntry* e = &cache->entries[cache->count];
                strlcpy(e->name, name, GOVEE_CACHE_NAME_LEN);
                memcpy(e->addr, a, 6);
                e->addr_type = (uint8_t)(addr_type_u & 0xFF);
                e->last_seen_ts = last_seen_ts;
                cache->count++;
            }
        }

        p = obj_end + 1;
    }

    free(buf);
    return true;
}

// ---------------------------------------------------------------------------
// save
// ---------------------------------------------------------------------------

bool govee_bulb_cache_save(GoveeBulbCache* cache, const char* path) {
    furi_check(cache != NULL);
    furi_check(path != NULL);

    Storage* storage = furi_record_open(RECORD_STORAGE);

    // ensure parent directory exists (non-fatal if already present)
    storage_common_mkdir(storage, GOVEE_CACHE_DIR);

    File* file = storage_file_alloc(storage);
    if(!storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        FURI_LOG_E(TAG, "Failed to open cache file for write: %s", path);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return false;
    }

    FuriString* out = furi_string_alloc();
    furi_string_cat_printf(out, "[\n");

    for(size_t i = 0; i < cache->count; i++) {
        const GoveeBulbCacheEntry* e = &cache->entries[i];
        furi_string_cat_printf(
            out,
            "  {"
            "\"name\": \"%s\", "
            "\"address\": \"%02X:%02X:%02X:%02X:%02X:%02X\", "
            "\"addr_type\": %u, "
            "\"last_seen_ts\": %lu"
            "}%s\n",
            e->name,
            e->addr[0], e->addr[1], e->addr[2],
            e->addr[3], e->addr[4], e->addr[5],
            (unsigned)e->addr_type,
            (unsigned long)e->last_seen_ts,
            (i + 1 < cache->count) ? "," : "");
    }

    furi_string_cat_printf(out, "]\n");

    const char* cstr = furi_string_get_cstr(out);
    size_t len = furi_string_size(out);
    bool ok = storage_file_write(file, cstr, len) == len;

    if(!ok) FURI_LOG_E(TAG, "Write error on cache file: %s", path);

    furi_string_free(out);
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    return ok;
}
