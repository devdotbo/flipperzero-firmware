/**
 * @file govee_registry.h
 * Lamp registry as pure data plus a JSON codec.
 *
 * No file I/O here. The glue owns the file steps: write bulbs.json.new with
 * every write and the close checked, move bulbs.json to bulbs.json.bak, move
 * .new into place; start-up order: valid bulbs.json, else valid .new, else
 * valid .bak; an invalid main file is kept as bulbs.json.corrupt.
 *
 * File format (identical to the old storage/bulb_cache.c writer, so files stay
 * readable by both the old and the new app):
 *
 *   [
 *     {"name": "ihoment_H6006_ABCD", "address": "A4:C1:38:12:34:56", "addr_type": 0, "last_seen_ts": 1700000000},
 *     {"name": "ihoment_H6006_EF01", "address": "A4:C1:38:65:43:21", "addr_type": 0, "last_seen_ts": 1700000000, "ok": 1},
 *     ...
 *   ]
 *
 * "ok" is written only when the result of the lamp's last completed
 * attempt is known: 1 succeeded, 0 failed. Absent means unknown, so files
 * without it load as before.
 *
 * Parsing rules (whole-file validation; any error rejects the whole file):
 * - top level is one JSON array of 0..GOVEE_MAX_LAMPS objects, only whitespace after it;
 * - each object needs "address" (string "XX:XX:XX:XX:XX:XX", hex, any case) and
 *   "addr_type" (integer 0..GOVEE_ADDR_TYPE_MAX); "name" (string) and
 *   "last_seen_ts" (integer 0..4294967295) and "ok" (integer 0 or 1) are
 *   optional (missing name: empty);
 * - unknown keys with scalar values (string, integer, true, false, null) are
 *   ignored (forward compatibility); nested objects or arrays are rejected;
 * - a known key twice in one object, duplicate identities, numbers with sign,
 *   fraction or exponent, raw control characters and embedded NUL are rejected;
 * - JSON string escapes are decoded; the name is then sanitised (see
 *   govee_registry_sanitize_name), so a name never makes a file invalid.
 *
 * Not thread-safe; the owner thread calls everything.
 */
#pragma once

#include "govee_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Upper bound for a serialised registry with GOVEE_MAX_LAMPS lamps (about 2.3 KiB used). */
#define GOVEE_REGISTRY_TEXT_MAX (4096u)

/** One saved lamp. */
typedef struct {
    char name[GOVEE_NAME_LEN]; /**< sanitised, NUL-terminated, may be empty */
    GoveeLampId id;
    uint32_t last_seen_ts; /**< RTC timestamp (seconds), 0 = unknown */
    uint8_t last; /**< GoveeLampLast; file key "ok": 1 Ok, 0 Failed, absent Unknown */
} GoveeRegistryLamp;

/** The registry: up to GOVEE_MAX_LAMPS lamps in insertion order. */
typedef struct {
    GoveeRegistryLamp lamps[GOVEE_MAX_LAMPS];
    uint8_t count;
} GoveeRegistry;

/** Result of govee_registry_upsert. */
typedef enum {
    GoveeRegistryAdded, /**< new identity appended */
    GoveeRegistryUpdated, /**< identity existed; name (if non-empty) and timestamp updated */
    GoveeRegistryFull, /**< new identity, but GOVEE_MAX_LAMPS lamps are saved already */
    GoveeRegistryInvalid, /**< NULL argument or address type above GOVEE_ADDR_TYPE_MAX */
} GoveeRegistryResult;

/** Result of govee_registry_parse. */
typedef enum {
    GoveeRegistryParseOk = 0,
    GoveeRegistryParseEmpty, /**< text is empty or whitespace only */
    GoveeRegistryParseSyntax, /**< not the expected JSON shape */
    GoveeRegistryParseMissingField, /**< object without address or addr_type */
    GoveeRegistryParseBadValue, /**< address, addr_type or last_seen_ts out of range */
    GoveeRegistryParseDuplicateKey, /**< the same key twice in one object */
    GoveeRegistryParseDuplicateLamp, /**< the same identity twice */
    GoveeRegistryParseTooMany, /**< more than GOVEE_MAX_LAMPS objects */
} GoveeRegistryParseResult;

/** Empty the registry. */
void govee_registry_init(GoveeRegistry* reg);

/**
 * Sanitise a name: printable ASCII 0x20..0x7E except '"' and '\\' is kept,
 * every other byte becomes '_'. Leading and trailing spaces are removed.
 * Output is truncated to dst_size - 1 bytes and always NUL-terminated.
 * src may be NULL (gives ""). dst_size must be >= 1.
 * @return length of dst
 */
size_t govee_registry_sanitize_name(char* dst, size_t dst_size, const char* src);

/**
 * Insert or update by identity (address type and address).
 * name is sanitised; an empty or NULL name never overwrites a stored name.
 * @param out_index  optional; receives the lamp index (0-based) for Added and Updated
 */
GoveeRegistryResult govee_registry_upsert(
    GoveeRegistry* reg,
    const char* name,
    const GoveeLampId* id,
    uint32_t last_seen_ts,
    uint8_t* out_index);

/** Remove the lamp at index (0-based); later lamps move up by one. @return false if out of range */
bool govee_registry_remove(GoveeRegistry* reg, uint8_t index);

/** @return index (0-based) of the identity or GOVEE_LAMP_NONE */
uint8_t govee_registry_find(const GoveeRegistry* reg, const GoveeLampId* id);

/**
 * Parse and validate file text. On success *out holds the lamps; on any error
 * *out is reset to empty, so parse into a scratch registry and copy on success
 * if the live one must survive a failed load.
 * @param text     file content, need not be NUL-terminated
 * @param len      number of bytes in text
 * @param err_pos  optional; receives the byte offset where parsing failed
 */
GoveeRegistryParseResult govee_registry_parse(
    GoveeRegistry* out,
    const char* text,
    size_t len,
    size_t* err_pos);

/**
 * Serialise into buf (always NUL-terminated when size >= 1).
 * @param out_len  optional; receives the number of bytes written without the NUL
 * @return false if the text did not fit (buf then holds a truncated, invalid file: do not save it)
 */
bool govee_registry_serialize(const GoveeRegistry* reg, char* buf, size_t size, size_t* out_len);

/** Format an address as "XX:XX:XX:XX:XX:XX" (buf needs 18 bytes). */
void govee_registry_format_addr(const uint8_t addr[GOVEE_ADDR_LEN], char buf[18]);

/** Short stable name of a parse result for logs and the status file. */
const char* govee_registry_parse_result_name(GoveeRegistryParseResult r);

#ifdef __cplusplus
}
#endif
