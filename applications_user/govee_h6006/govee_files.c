/**
 * @file govee_files.c
 * File steps on the app thread: registry load with recovery,
 * registry save through .new and .bak, and the remote status file.
 * Every write and close is checked; failures are logged, shown and counted.
 */
#include "govee_h6006_app.h"

#include <stdio.h>
#include <string.h>

typedef enum {
    ReadMissing,
    ReadOk,
    ReadInvalid,
    ReadError, /**< exists but could not be opened or read */
    ReadProbeError, /**< the storage could not say whether the file exists */
} ReadResult;

/* Exists, missing, or a storage error: only FSE_NOT_EXIST means missing. */
static ReadResult probe_file(GoveeH6006App* app, const char* path) {
    FileInfo info;
    FS_Error err = storage_common_stat(app->storage, path, &info);
    if(err == FSE_OK) return file_info_is_dir(&info) ? ReadMissing : ReadOk;
    if(err == FSE_NOT_EXIST) return ReadMissing;
    FURI_LOG_E(GOVEE_TAG, "probing %s failed: %s", path, storage_error_get_desc(err));
    return ReadProbeError;
}

/* Read and validate one candidate file into app->scratch (uses app->text). */
static ReadResult read_registry(GoveeH6006App* app, const char* path) {
    ReadResult probe = probe_file(app, path);
    if(probe != ReadOk) return probe;

    File* file = storage_file_alloc(app->storage);
    ReadResult result = ReadError;
    do {
        if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) break;
        uint64_t size = storage_file_size(file);
        if(size > GOVEE_REGISTRY_TEXT_MAX) {
            FURI_LOG_W(GOVEE_TAG, "%s too large (%lu bytes)", path, (uint32_t)size);
            result = ReadInvalid;
            break;
        }
        size_t want = (size_t)size;
        size_t got = storage_file_read(file, app->text, want);
        if(got != want) break;
        size_t err_pos = 0;
        GoveeRegistryParseResult pr = govee_registry_parse(app->scratch, app->text, got, &err_pos);
        if(pr == GoveeRegistryParseOk) {
            result = ReadOk;
        } else {
            FURI_LOG_W(
                GOVEE_TAG,
                "%s invalid: %s at byte %u",
                path,
                govee_registry_parse_result_name(pr),
                (unsigned)err_pos);
            result = ReadInvalid;
        }
    } while(false);
    storage_file_close(file);
    storage_file_free(file);
    return result;
}

/* Write buf to path (created or truncated); every write and the close checked. */
static bool write_file(GoveeH6006App* app, const char* path, const void* buf, size_t len) {
    File* file = storage_file_alloc(app->storage);
    bool ok = storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(ok) {
        if(storage_file_write(file, buf, len) != len) ok = false;
        if(!storage_file_close(file)) ok = false;
    }
    storage_file_free(file);
    return ok;
}

/* Read path back and compare it with buf[0..len). */
static bool file_matches(GoveeH6006App* app, const char* path, const char* buf, size_t len) {
    File* file = storage_file_alloc(app->storage);
    bool ok = storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING) &&
              storage_file_size(file) == len;
    uint8_t chunk[64];
    size_t pos = 0;
    while(ok && pos < len) {
        size_t want = len - pos;
        if(want > sizeof(chunk)) want = sizeof(chunk);
        if(storage_file_read(file, chunk, want) != want || memcmp(chunk, buf + pos, want) != 0) {
            ok = false;
        }
        pos += want;
    }
    storage_file_close(file);
    storage_file_free(file);
    return ok;
}

/* Copy src to dst in chunks (app->text as buffer), then verify dst against src
 * by reading both back. The source is never modified. */
static bool copy_verified(GoveeH6006App* app, const char* src, const char* dst) {
    File* in = storage_file_alloc(app->storage);
    File* out = storage_file_alloc(app->storage);
    uint64_t size = 0;
    bool ok = storage_file_open(in, src, FSAM_READ, FSOM_OPEN_EXISTING);
    if(ok) {
        size = storage_file_size(in);
        ok = storage_file_open(out, dst, FSAM_WRITE, FSOM_CREATE_ALWAYS);
        if(ok) {
            for(uint64_t done = 0; ok && done < size;) {
                size_t want = (size - done > GOVEE_REGISTRY_TEXT_MAX) ? GOVEE_REGISTRY_TEXT_MAX :
                                                                       (size_t)(size - done);
                if(storage_file_read(in, app->text, want) != want ||
                   storage_file_write(out, app->text, want) != want) {
                    ok = false;
                }
                done += want;
            }
            if(!storage_file_close(out)) ok = false;
        }
    }
    storage_file_close(in);

    /* verify: same size and same bytes */
    if(ok) ok = storage_file_open(in, src, FSAM_READ, FSOM_OPEN_EXISTING);
    if(ok) {
        ok = storage_file_open(out, dst, FSAM_READ, FSOM_OPEN_EXISTING) &&
             storage_file_size(out) == size;
        uint8_t chunk[64];
        for(uint64_t done = 0; ok && done < size;) {
            size_t want = (size - done > GOVEE_REGISTRY_TEXT_MAX) ? GOVEE_REGISTRY_TEXT_MAX :
                                                                   (size_t)(size - done);
            if(storage_file_read(in, app->text, want) != want) {
                ok = false;
                break;
            }
            for(size_t pos = 0; ok && pos < want;) {
                size_t n = want - pos;
                if(n > sizeof(chunk)) n = sizeof(chunk);
                if(storage_file_read(out, chunk, n) != n ||
                   memcmp(chunk, app->text + pos, n) != 0) {
                    ok = false;
                }
                pos += n;
            }
            done += want;
        }
        storage_file_close(out);
    }
    storage_file_close(in);
    storage_file_free(out);
    storage_file_free(in);
    return ok;
}

/* Keep an invalid main file as bulbs.json.corrupt (or .corrupt.1 .. .9); never
 * overwrite one. The main file is removed only after the copy was verified. */
static bool preserve_corrupt(GoveeH6006App* app) {
    char path[sizeof(GOVEE_APP_BULBS_BAD) + 4];
    for(uint8_t i = 0; i < 10; i++) {
        if(i == 0) {
            snprintf(path, sizeof(path), "%s", GOVEE_APP_BULBS_BAD);
        } else {
            snprintf(path, sizeof(path), "%s.%u", GOVEE_APP_BULBS_BAD, i);
        }
        ReadResult probe = probe_file(app, path);
        if(probe == ReadOk) continue;
        if(probe != ReadMissing) return false;
        if(!copy_verified(app, GOVEE_APP_BULBS, path)) {
            FURI_LOG_E(GOVEE_TAG, "keeping invalid bulbs.json as %s failed", path);
            return false;
        }
        FS_Error err = storage_common_remove(app->storage, GOVEE_APP_BULBS);
        if(err != FSE_OK) {
            /* the verified copy exists; a later save overwrites the invalid main file */
            FURI_LOG_W(GOVEE_TAG, "removing invalid bulbs.json failed: %s", storage_error_get_desc(err));
        }
        FURI_LOG_W(GOVEE_TAG, "invalid bulbs.json kept as %s", path);
        return true;
    }
    FURI_LOG_E(GOVEE_TAG, "no free .corrupt name");
    return false;
}

static void load_probe_failed(GoveeH6006App* app) {
    govee_registry_init(app->registry);
    app->save_blocked = true;
    app->load_result = "probe_error";
    govee_ui_notice(app, "The SD card could not be read. Lamps not loaded, saving is off.");
}

/* A recovery file that exists but cannot be read may hold the newest
 * registry; do not replace anything this session, as for the main file. */
static void recovery_read_failed(GoveeH6006App* app, bool* read_error, const char* name) {
    *read_error = true;
    app->save_blocked = true;
    char text[64];
    snprintf(text, sizeof(text), "%s could not be read. Saving is off.", name);
    govee_ui_notice(app, text);
}

void govee_files_load(GoveeH6006App* app) {
    storage_simply_mkdir(app->storage, GOVEE_APP_DIR);
    govee_registry_init(app->registry);
    snprintf(app->save_result, sizeof(app->save_result), "none");

    bool any_invalid = false;
    bool probe_error = false;
    bool read_error = false; /* a file exists but could not be read (main, .new or .bak) */
    ReadResult main_result = read_registry(app, GOVEE_APP_BULBS);
    if(main_result == ReadOk) {
        *app->registry = *app->scratch;
        app->load_result = "main";
    } else if(main_result == ReadProbeError) {
        probe_error = true;
    } else {
        if(main_result == ReadInvalid) {
            any_invalid = true;
            if(!preserve_corrupt(app)) {
                app->save_blocked = true;
                govee_ui_notice(app, "bulbs.json is invalid and could not be kept. Saving is off.");
            }
        } else if(main_result == ReadError) {
            /* exists but unreadable: do not replace it this session */
            read_error = true;
            app->save_blocked = true;
            govee_ui_notice(app, "bulbs.json could not be read. Saving is off.");
        }

        ReadResult r = read_registry(app, GOVEE_APP_BULBS_NEW);
        if(r == ReadOk) {
            *app->registry = *app->scratch;
            app->load_result = "new";
        } else if(r == ReadProbeError) {
            probe_error = true;
        } else {
            if(r == ReadError) recovery_read_failed(app, &read_error, "bulbs.json.new");
            if(r == ReadInvalid) any_invalid = true;
            r = read_registry(app, GOVEE_APP_BULBS_BAK);
            if(r == ReadOk) {
                *app->registry = *app->scratch;
                app->load_result = "bak";
            } else if(r == ReadProbeError) {
                probe_error = true;
            } else {
                if(r == ReadError) recovery_read_failed(app, &read_error, "bulbs.json.bak");
                if(r == ReadInvalid) any_invalid = true;
                app->load_result = read_error                 ? "read_error" :
                                   any_invalid                ? "none_valid" :
                                                                "empty";
            }
        }
    }
    if(probe_error) load_probe_failed(app);
    if(app->save_blocked) snprintf(app->save_result, sizeof(app->save_result), "blocked");
    govee_registry_init(app->scratch);
    FURI_LOG_I(GOVEE_TAG, "registry loaded from %s: %u lamps", app->load_result, app->registry->count);

    /* Recovered from .new or .bak: write a valid bulbs.json again. bulbs.json is
     * missing or invalid here, so the save writes it directly and leaves .new
     * and .bak untouched (the source of the recovery is never truncated). */
    if(strcmp(app->load_result, "new") == 0 || strcmp(app->load_result, "bak") == 0) {
        govee_files_save(app);
    }
}

static void save_failed(GoveeH6006App* app, const char* step) {
    app->save_failures++;
    snprintf(
        app->save_result, sizeof(app->save_result), "%s fails=%lu", step, app->save_failures);
    FURI_LOG_E(GOVEE_TAG, "saving lamps failed: %s", step);
    char text[64];
    snprintf(text, sizeof(text), "Saving lamps failed (%s).", step);
    govee_ui_notice(app, text);
}

/* Serialize the registry into app->text. */
static bool serialize(GoveeH6006App* app, size_t* len) {
    return govee_registry_serialize(app->registry, app->text, GOVEE_REGISTRY_TEXT_MAX, len);
}

/* Save: no valid copy is given up on a storage call's return value alone.
 *  0. Probe bulbs.json (read and parse). A storage error fails the save.
 *  If bulbs.json is valid:
 *  1. write bulbs.json.new, close, read back and compare;
 *  2. copy bulbs.json to bulbs.json.bak, compare, read back and parse;
 *  3. write bulbs.json, read back and compare.
 *  If bulbs.json is missing or invalid it holds nothing to keep: only step 3
 *  runs, and .new and .bak (a possible recovery source) stay untouched.
 *  A failure at step 3 leaves the previous version in .bak and the new one in .new. */
bool govee_files_save(GoveeH6006App* app) {
    if(app->save_blocked) {
        save_failed(app, "blocked");
        return false;
    }
    storage_simply_mkdir(app->storage, GOVEE_APP_DIR);

    ReadResult main_result = read_registry(app, GOVEE_APP_BULBS); /* uses app->text */
    if(main_result == ReadProbeError || main_result == ReadError) {
        save_failed(app, "probe");
        return false;
    }
    size_t len = 0;
    if(!serialize(app, &len)) {
        save_failed(app, "serialize");
        return false;
    }

    if(main_result == ReadOk) {
        /* 1. bulbs.json.new */
        if(!write_file(app, GOVEE_APP_BULBS_NEW, app->text, len)) {
            save_failed(app, "new write");
            return false;
        }
        if(!file_matches(app, GOVEE_APP_BULBS_NEW, app->text, len)) {
            save_failed(app, "new verify");
            return false;
        }
        /* 2. bulbs.json.bak from the current (valid) bulbs.json */
        if(!copy_verified(app, GOVEE_APP_BULBS, GOVEE_APP_BULBS_BAK)) {
            save_failed(app, "bak copy");
            return false;
        }
        if(read_registry(app, GOVEE_APP_BULBS_BAK) != ReadOk) {
            save_failed(app, "bak verify");
            return false;
        }
        govee_registry_init(app->scratch);
        if(!serialize(app, &len)) { /* app->text was used as the copy buffer */
            save_failed(app, "serialize");
            return false;
        }
    }

    /* 3. bulbs.json */
    if(!write_file(app, GOVEE_APP_BULBS, app->text, len)) {
        save_failed(app, "main write");
        return false;
    }
    if(!file_matches(app, GOVEE_APP_BULBS, app->text, len)) {
        save_failed(app, "main verify");
        return false;
    }

    app->results_unsaved = false; /* the file now holds every lamp result */
    if(app->save_failures == 0) {
        snprintf(app->save_result, sizeof(app->save_result), "ok");
    } else {
        snprintf(app->save_result, sizeof(app->save_result), "ok fails=%lu", app->save_failures);
    }
    FURI_LOG_D(GOVEE_TAG, "saved %u lamps (%u bytes)", app->registry->count, (unsigned)len);
    return true;
}

void govee_files_write_status(GoveeH6006App* app, uint32_t req) {
    app->dump_seq++;
    GoveeStatusInput in = {
        .seq = app->dump_seq,
        .session = app->session,
        .req = req,
        .now_ms = furi_get_tick(),
        .app_state = govee_ui_state_token(app),
        .inbox_overflow = atomic_load(&app->inbox_overflow),
        .target = app->target,
        .scanning = govee_link_scan_active(app),
        .scan_refused = app->scan_refused,
        .lightshow = app->lightshow,
        .registry_full = app->registry_full,
        .load_result = app->load_result,
        .save_result = app->save_result,
        .registry = app->registry,
        .engine = app->engine,
    };

    storage_simply_mkdir(app->storage, GOVEE_APP_DIR);
    File* file = storage_file_alloc(app->storage);
    bool ok = storage_file_open(file, GOVEE_APP_STATUS_TMP, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(ok) {
        uint32_t cursor = 0;
        /* bounded: header, 16 lamps, 64 events, end; the formatter ends with Done */
        for(uint32_t lines = 0; lines < 256u; lines++) {
            GoveeStatusLineResult r =
                govee_status_format_line(&in, &cursor, app->line, GOVEE_STATUS_LINE_MAX);
            if(r == GoveeStatusLineDone) break;
            size_t len = strlen(app->line);
            if(storage_file_write(file, app->line, len) != len) {
                ok = false;
                break;
            }
        }
        if(!storage_file_close(file)) ok = false;
    }
    storage_file_free(file);
    if(!ok) {
        app->status_publish_retries = 0;
        app->dump_failures++;
        FURI_LOG_E(GOVEE_TAG, "status %lu write failed (%lu)", app->dump_seq, app->dump_failures);
        return;
    }
    app->status_publish_retries = GOVEE_STATUS_PUBLISH_RETRIES;
    govee_files_status_publish(app);
}

/* Move the finished status file into place. A reader holding the old file
 * makes the move fail; the app tick retries a few times. */
void govee_files_status_publish(GoveeH6006App* app) {
    if(app->status_publish_retries == 0) return;
    app->status_publish_retries--;
    FS_Error err = storage_common_rename(app->storage, GOVEE_APP_STATUS_TMP, GOVEE_STATUS_PATH);
    if(err == FSE_OK) {
        app->status_publish_retries = 0;
        FURI_LOG_D(GOVEE_TAG, "status %lu written", app->dump_seq);
        return;
    }
    if(app->status_publish_retries > 0) {
        FURI_LOG_W(
            GOVEE_TAG,
            "status %lu move failed (%s), retrying",
            app->dump_seq,
            storage_error_get_desc(err));
    } else {
        app->dump_failures++;
        FURI_LOG_E(GOVEE_TAG, "status %lu write failed (%lu)", app->dump_seq, app->dump_failures);
    }
}
